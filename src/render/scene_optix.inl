
#include <iomanip>
#include <cstring>
#include <cstdio>

#include <drjit-core/optix.h>

#include <nanothread/nanothread.h>

#include <mitsuba/render/optix/common.h>
#include <mitsuba/render/optix/shapes.h>
#include <mitsuba/render/optix_api.h>

#include "librender_ptx.h"

NAMESPACE_BEGIN(mitsuba)

#define MI_OPTIX_ENABLE_BBOX_FASTPATH

#if !defined(NDEBUG)
#  define MI_OPTIX_DEBUG 1
#endif

#ifdef _MSC_VER
#  define strdup(x) _strdup(x)
#endif

static constexpr size_t ProgramGroupCount = 2 + custom_optix_shapes_count;

// Per scene OptiX state data structure
struct OptixSceneState {
    OptixShaderBindingTable sbt = {};
    OptixAccelData accel;
    OptixTraversableHandle ias_handle = 0ull;
    void* ias_buffer = nullptr;
    size_t config_index;
};

/**
 * \brief Optix configuration data structure
 *
 * OptiX modules and program groups can be compiled with different set of
 * features and optimizations, which might vary depending on the scene's
 * requirements. This data structure hold those OptiX pipeline components for a
 * specific configuration, which can be shared across multiple scenes.
 *
 * \ref Scene::static_accel_shutdown is responsible for freeing those programs.
 */
struct OptixConfig {
    OptixDeviceContext context;
    OptixPipelineCompileOptions pipeline_compile_options;
    OptixModule module;
    OptixProgramGroup program_groups[ProgramGroupCount];
    char *custom_shapes_program_names[2 * custom_optix_shapes_count];
};

// Array storing previously initialized optix configurations
static OptixConfig optix_configs[8] = {};

size_t init_optix_config(bool has_meshes, bool has_others, bool has_instances) {
    // Compute config index in optix_configs based on required set of features
    size_t config_index =
        (has_instances ? 4 : 0) +
        (has_meshes ? 2 : 0) +
        (has_others ? 1 : 0);

    OptixConfig &config = optix_configs[config_index];

    // Initialize Optix config if necessary
    if (!config.module) {
        Log(Debug, "Initialize Optix configuration (index=%zu)..", config_index);

        config.context = jit_optix_context();

        // =====================================================
        // Configure options for OptiX pipeline
        // =====================================================

        OptixModuleCompileOptions module_compile_options { };
        module_compile_options.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    #if !defined(MI_OPTIX_DEBUG)
        module_compile_options.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
        module_compile_options.debugLevel       = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    #else
        module_compile_options.optLevel         = OPTIX_COMPILE_OPTIMIZATION_LEVEL_0;
        module_compile_options.debugLevel       = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
    #endif

        config.pipeline_compile_options.usesMotionBlur     = false;
        config.pipeline_compile_options.numPayloadValues   = 6;
        config.pipeline_compile_options.numAttributeValues = 2; // the minimum legal value
        config.pipeline_compile_options.pipelineLaunchParamsVariableName = "params";

        if (has_instances)
            config.pipeline_compile_options.traversableGraphFlags =
                OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
        else if (has_others && has_meshes)
            config.pipeline_compile_options.traversableGraphFlags =
                OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        else
            config.pipeline_compile_options.traversableGraphFlags =
                OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;

    #if !defined(MI_OPTIX_DEBUG)
        config.pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    #else
        config.pipeline_compile_options.exceptionFlags =
                OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW
                | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH
                | OPTIX_EXCEPTION_FLAG_DEBUG;
    #endif

        unsigned int prim_flags = 0;
        if (has_meshes)
            prim_flags |= OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
        if (has_others)
            prim_flags |= OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;

        config.pipeline_compile_options.usesPrimitiveTypeFlags = prim_flags;

        // =====================================================
        // Logging infrastructure for pipeline setup
        // =====================================================

        char optix_log[2048];
        size_t optix_log_size = sizeof(optix_log);
        auto check_log = [&](int rv) {
            if (rv) {
                fprintf(stderr, "\tLog: %s%s", optix_log,
                        optix_log_size > sizeof(optix_log) ? "<TRUNCATED>" : "");
                jit_optix_check(rv);
            }
        };

        // =====================================================
        // Create Optix module from supplemental PTX code
        // =====================================================

        OptixTask task;
        check_log(optixModuleCreateFromPTXWithTasks(
            config.context,
            &module_compile_options,
            &config.pipeline_compile_options,
            (const char *)optix_rt_ptx,
            optix_rt_ptx_size,
            optix_log,
            &optix_log_size,
            &config.module,
            &task
        ));

        std::function<void(OptixTask)> execute_task = [&](OptixTask task) {
            unsigned int max_new_tasks = pool_size();

            std::unique_ptr<OptixTask[]> new_tasks =
                std::make_unique<OptixTask[]>(max_new_tasks);
            unsigned int new_task_count = 0;
            optixTaskExecute(task, new_tasks.get(), max_new_tasks,
                             &new_task_count);

            parallel_for(
                drjit::blocked_range<size_t>(0, new_task_count, 1),
                [&](const drjit::blocked_range<size_t> &range) {
                    for (auto i = range.begin(); i != range.end(); ++i) {
                        OptixTask new_task = new_tasks[i];
                        execute_task(new_task);
                    }
                }
            );
        };
        execute_task(task);

        int compilation_state = 0;
        check_log(
            optixModuleGetCompilationState(config.module, &compilation_state));
        if (compilation_state != OPTIX_MODULE_COMPILE_STATE_COMPLETED)
            Throw("Optix configuration initialization failed! The OptiX module "
                  "compilation did not complete succesfully. The module's "
                  "compilation state is: %#06x", compilation_state);

        // =====================================================
        // Create program groups (raygen provided by Dr.Jit..)
        // =====================================================

        OptixProgramGroupOptions program_group_options = {};
        OptixProgramGroupDesc pgd[ProgramGroupCount] {};

        pgd[0].kind                         = OPTIX_PROGRAM_GROUP_KIND_MISS;
        pgd[0].miss.module                  = config.module;
        pgd[0].miss.entryFunctionName       = "__miss__ms";
        pgd[1].kind                         = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        pgd[1].hitgroup.moduleCH            = config.module;
        pgd[1].hitgroup.entryFunctionNameCH = "__closesthit__mesh";

        for (size_t i = 0; i < custom_optix_shapes_count; i++) {
            pgd[2+i].kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;

            std::string name = string::to_lower(custom_optix_shapes[i]);
            config.custom_shapes_program_names[2*i]   = strdup(("__closesthit__"   + name).c_str());
            config.custom_shapes_program_names[2*i+1] = strdup(("__intersection__" + name).c_str());

            pgd[2+i].hitgroup.moduleCH            = config.module;
            pgd[2+i].hitgroup.entryFunctionNameCH = config.custom_shapes_program_names[2*i];
            pgd[2+i].hitgroup.moduleIS            = config.module;
            pgd[2+i].hitgroup.entryFunctionNameIS = config.custom_shapes_program_names[2*i+1];
        }

        optix_log_size = sizeof(optix_log);
        check_log(optixProgramGroupCreate(
            config.context,
            pgd,
            ProgramGroupCount,
            &program_group_options,
            optix_log,
            &optix_log_size,
            config.program_groups
        ));
    }

    return config_index;
}

MI_VARIANT void Scene<Float, Spectrum>::accel_init_gpu(const Properties &/*props*/) {
    if constexpr (dr::is_cuda_v<Float>) {
        ScopedPhase phase(ProfilerPhase::InitAccel);
        Log(Info, "Building scene in OptiX ..");
        Timer timer;
        optix_initialize();

        m_accel = new OptixSceneState();
        OptixSceneState &s = *(OptixSceneState *) m_accel;

        // =====================================================
        //  Initialize OptiX configuration
        // =====================================================

        bool has_meshes = false;
        bool has_others = false;
        bool has_instances = false;

        for (auto& shape : m_shapes) {
            has_meshes    |= shape->is_mesh();
            has_others    |= !shape->is_mesh() && !shape->is_instance();
            has_instances |= shape->is_instance();
        }

        for (auto& shape : m_shapegroups) {
            has_meshes |= !shape->has_meshes();
            has_others |= !shape->has_others();
        }

        s.config_index = init_optix_config(has_meshes, has_others, has_instances);
        const OptixConfig &config = optix_configs[s.config_index];

        // =====================================================
        //  Shader Binding Table generation
        // =====================================================

        std::vector<HitGroupSbtRecord> hg_sbts;
        fill_hitgroup_records(m_shapes, hg_sbts, config.program_groups);
        for (auto& shapegroup: m_shapegroups)
            shapegroup->optix_fill_hitgroup_records(hg_sbts, config.program_groups);

        size_t shapes_count = hg_sbts.size();

        s.sbt.missRecordBase =
            jit_malloc(AllocType::HostPinned, sizeof(MissSbtRecord));
        s.sbt.missRecordStrideInBytes = sizeof(MissSbtRecord);
        s.sbt.missRecordCount = 1;

        s.sbt.hitgroupRecordBase = jit_malloc(
            AllocType::HostPinned, shapes_count * sizeof(HitGroupSbtRecord));
        s.sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupSbtRecord);
        s.sbt.hitgroupRecordCount = (unsigned int) shapes_count;

        jit_optix_check(optixSbtRecordPackHeader(config.program_groups[0],
                                                 s.sbt.missRecordBase));

        jit_memcpy_async(JitBackend::CUDA, s.sbt.hitgroupRecordBase, hg_sbts.data(),
                         shapes_count * sizeof(HitGroupSbtRecord));

        s.sbt.missRecordBase =
            jit_malloc_migrate(s.sbt.missRecordBase, AllocType::Device, 1);
        s.sbt.hitgroupRecordBase =
            jit_malloc_migrate(s.sbt.hitgroupRecordBase, AllocType::Device, 1);

        // =====================================================
        //  Acceleration data structure building
        // =====================================================

        accel_parameters_changed_gpu();

         Log(Info, "OptiX ready. (took %s)", util::time_string((float) timer.value()));
    }
}

MI_VARIANT void Scene<Float, Spectrum>::accel_parameters_changed_gpu() {
    if constexpr (dr::is_cuda_v<Float>) {
        dr::sync_thread();
        OptixSceneState &s = *(OptixSceneState *) m_accel;
        const OptixConfig &config = optix_configs[s.config_index];

        if (!m_shapes.empty()) {
            // Build geometry acceleration structures for all the shapes
            build_gas(config.context, m_shapes, s.accel);
            for (auto& shapegroup: m_shapegroups)
                shapegroup->optix_build_gas(config.context);

            // Gather information about the instance acceleration structures to be built
            std::vector<OptixInstance> ias;
            prepare_ias(config.context, m_shapes, 0, s.accel, 0u, ScalarTransform4f(), ias);

            // If there is only a single IAS, no need to build the "master" IAS
            if (ias.size() == 1) {
                s.ias_buffer = nullptr;
                s.ias_handle = ias[0].traversableHandle;
            } else {
                // Build a "master" IAS that contains all the IAS of the scene (meshes,
                // custom shapes, instances, ...)
                OptixAccelBuildOptions accel_options = {};
                accel_options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
                accel_options.operation  = OPTIX_BUILD_OPERATION_BUILD;
                accel_options.motionOptions.numKeys = 0;

                size_t ias_data_size = ias.size() * sizeof(OptixInstance);
                void* d_ias = jit_malloc(AllocType::HostPinned, ias_data_size);
                jit_memcpy_async(JitBackend::CUDA, d_ias, ias.data(), ias_data_size);

                OptixBuildInput build_input;
                build_input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
                build_input.instanceArray.instances =
                    (CUdeviceptr) jit_malloc_migrate(d_ias, AllocType::Device, 1);
                build_input.instanceArray.numInstances = (unsigned int) ias.size();

                OptixAccelBufferSizes buffer_sizes;
                jit_optix_check(optixAccelComputeMemoryUsage(
                    config.context,
                    &accel_options,
                    &build_input,
                    1,
                    &buffer_sizes
                ));

                void* d_temp_buffer
                    = jit_malloc(AllocType::Device, buffer_sizes.tempSizeInBytes);
                s.ias_buffer
                    = jit_malloc(AllocType::Device, buffer_sizes.outputSizeInBytes);

                jit_optix_check(optixAccelBuild(
                    config.context,
                    (CUstream) jit_cuda_stream(),
                    &accel_options,
                    &build_input,
                    1, // num build inputs
                    (CUdeviceptr)d_temp_buffer,
                    buffer_sizes.tempSizeInBytes,
                    (CUdeviceptr)s.ias_buffer,
                    buffer_sizes.outputSizeInBytes,
                    &s.ias_handle,
                    0, // emitted property list
                    0  // num emitted properties
                ));

                jit_free(d_temp_buffer);
            }
        }

        /* Set up a callback on the handle variable to release the OptiX scene
           state when this variable is freed. This ensures that the lifetime of
           the pipeline goes beyond the one of the Scene instance if there are
           still some pending ray tracing calls (e.g. non evaluated variables
           depending on a ray tracing call). */

        // Prevents the pipeline to be released when updating the scene parameters
        if (m_accel_handle.index())
            jit_var_set_callback(m_accel_handle.index(), nullptr, nullptr);
        m_accel_handle = dr::opaque<UInt64>(s.ias_handle);

        jit_var_set_callback(
            m_accel_handle.index(),
            [](uint32_t /* index */, int should_free, void *payload) {
                OptixSceneState *s = (OptixSceneState *) payload;
                if (should_free) {
                    Log(Debug, "Free OptiX scene state..");

                    jit_free(s->sbt.raygenRecord);
                    jit_free(s->sbt.hitgroupRecordBase);
                    jit_free(s->sbt.missRecordBase);
                    jit_free(s->ias_buffer);

                    delete s;
                }
            },
            (void *) m_accel
        );

        clear_shapes_dirty();
    }
}

MI_VARIANT void Scene<Float, Spectrum>::accel_release_gpu() {
    if constexpr (dr::is_cuda_v<Float>) {
        // Ensure all ray tracing kernels are terminated before releasing the scene
        dr::sync_thread();

        /* Decrease the reference count of the handle variable. This will
           trigger the release of the OptiX acceleration data structure if no
           ray tracing calls are pending. */
        m_accel_handle = 0;
        m_accel = nullptr;
    }
}

MI_VARIANT void Scene<Float, Spectrum>::static_accel_initialization_gpu() { }
MI_VARIANT void Scene<Float, Spectrum>::static_accel_shutdown_gpu() {
    Log(Debug, "Optix configuration shutdown..");
    for (size_t j = 0; j < 8; j++) {
        OptixConfig &config = optix_configs[j];
        if (config.module) {
            for (size_t i = 0; i < ProgramGroupCount; i++)
                jit_optix_check(optixProgramGroupDestroy(config.program_groups[i]));
            for (size_t i = 0; i < 2 * custom_optix_shapes_count; i++)
                free(config.custom_shapes_program_names[i]);
            jit_optix_check(optixModuleDestroy(config.module));
            config.module = nullptr;
        }
    }
}

MI_VARIANT typename Scene<Float, Spectrum>::PreliminaryIntersection3f
Scene<Float, Spectrum>::ray_intersect_preliminary_gpu(const Ray3f &ray,
                                                      Mask active) const {
    if constexpr (dr::is_cuda_v<Float>) {
        OptixSceneState &s = *(OptixSceneState *) m_accel;
        const OptixConfig &config = optix_configs[s.config_index];

        // Override optix configuration in drjit-core.
        // TODO This could be problematic when raytracing calls on other scenes are still pending.
        jit_optix_configure(
            &config.pipeline_compile_options,
            &s.sbt,
            config.program_groups, ProgramGroupCount
        );

        UInt32 ray_mask(255), ray_flags(OPTIX_RAY_FLAG_NONE),
               sbt_offset(0), sbt_stride(1), miss_sbt_index(0);

        UInt32 payload_t(0),
               payload_prim_u(0),
               payload_prim_v(0),
               payload_prim_index(0),
               payload_shape_ptr(0);

        // Instance index is initialized to 0 when there is no instancing in the scene
        UInt32 payload_inst_index(m_shapegroups.empty() ? 0u : 1u);

        using Single = dr::float32_array_t<Float>;
        dr::Array<Single, 3> ray_o(ray.o), ray_d(ray.d);
        Single ray_mint(0.f), ray_maxt(ray.maxt), ray_time(ray.time);

        // Be careful with 'ray.maxt' in double precision variants
        if constexpr (!std::is_same_v<Single, Float>)
            ray_maxt = dr::minimum(ray_maxt, dr::Largest<Single>);

        uint32_t trace_args[] {
            m_accel_handle.index(),
            ray_o.x().index(), ray_o.y().index(), ray_o.z().index(),
            ray_d.x().index(), ray_d.y().index(), ray_d.z().index(),
            ray_mint.index(), ray_maxt.index(), ray_time.index(),
            ray_mask.index(), ray_flags.index(),
            sbt_offset.index(), sbt_stride.index(),
            miss_sbt_index.index(),
            payload_t.index(),
            payload_prim_u.index(),
            payload_prim_v.index(),
            payload_prim_index.index(),
            payload_shape_ptr.index(),
            payload_inst_index.index(),
        };

        jit_optix_ray_trace(sizeof(trace_args) / sizeof(uint32_t),
                            trace_args, active.index());

        PreliminaryIntersection3f pi;
        pi.t          = dr::reinterpret_array<Single, UInt32>(UInt32::steal(trace_args[15]));
        pi.prim_uv[0] = dr::reinterpret_array<Single, UInt32>(UInt32::steal(trace_args[16]));
        pi.prim_uv[1] = dr::reinterpret_array<Single, UInt32>(UInt32::steal(trace_args[17]));
        pi.prim_index = UInt32::steal(trace_args[18]);
        pi.shape      = ShapePtr::steal(trace_args[19]);
        pi.instance   = ShapePtr::steal(trace_args[20]);

        // This field is only used by embree, but we still need to initialize it for vcalls
        pi.shape_index = dr::zeros<UInt32>();

        // jit_optix_ray_trace leaves payload data uninitialized for inactive lanes
        pi.t[!active] = dr::Infinity<Float>;

        // Ensure pointers are initialized to nullptr for inactive lanes
        active &= pi.is_valid();
        pi.shape[!active]    = nullptr;
        pi.instance[!active] = nullptr;

        return pi;
    } else {
        DRJIT_MARK_USED(ray);
        DRJIT_MARK_USED(active);
        Throw("ray_intersect_gpu() should only be called in GPU mode.");
    }
}

MI_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_gpu(const Ray3f &ray, uint32_t ray_flags,
                                          Mask active) const {
    if constexpr (dr::is_cuda_v<Float>) {
#if defined(MI_OPTIX_ENABLE_BBOX_FASTPATH)
        // Attempt a fast path for scenes that are only made of a single AABB.
        if (m_use_bbox_fast_path) {
            SurfaceInteraction3f si = dr::zeros<SurfaceInteraction3f>();

            const ref<Shape> shape = m_shapes[0];
            const ScalarBoundingBox3f bbox = shape->bbox();

            auto [hit, mint, maxt] = bbox.ray_intersect(ray);
            Mask starts_outside = mint > 0.f;
            Float t = dr::select(starts_outside, mint, maxt);
            hit &= active && (t <= ray.maxt) && (t > math::RayEpsilon<Float>);
            si.t = dr::select(hit, t, dr::Infinity<Float>);

            si.time = ray.time;
            si.wavelengths = ray.wavelengths;
            si.p = ray(si.t);

            // Normal vector: assuming axis-aligned bbox, figure
            // out the normal direction based on the relative position
            // of the intersection point to the bbox's center.
            Point3f p_local = (si.p - bbox.center()) / bbox.extents();
            // The axis with the largest local coordinate (magnitude)
            // is the axis of the normal vector.
            Point3f p_local_abs = dr::abs(p_local);
            Float vmax = dr::max(p_local_abs);
            Normal3f n(dr::eq(p_local_abs.x(), vmax), dr::eq(p_local_abs.y(), vmax),
                    dr::eq(p_local_abs.z(), vmax));
            // Normal always points to the outside of the bbox, independently
            // of the ray direction.
            n = dr::normalize(dr::sign(p_local) * n);
            si.n = dr::select(hit, n, -ray.d);

            si.shape = dr::select(hit, dr::opaque<ShapePtr>(shape.get()), dr::zeros<ShapePtr>());
            si.uv = 0.f;  // TODO: proper UVs
            si.sh_frame.n = si.n;
            if (has_flag(ray_flags, RayFlags::ShadingFrame))
                si.initialize_sh_frame();
            si.wi = dr::select(hit, si.to_local(-ray.d), -ray.d);
            return si;
        } else {
            PreliminaryIntersection3f pi = ray_intersect_preliminary_gpu(ray, active);
            return pi.compute_surface_interaction(ray, ray_flags, active);
        }
#else
        PreliminaryIntersection3f pi = ray_intersect_preliminary_gpu(ray, active);
        return pi.compute_surface_interaction(ray, ray_flags, active);
#endif
    } else {
        DRJIT_MARK_USED(ray);
        DRJIT_MARK_USED(ray_flags);
        DRJIT_MARK_USED(active);
        Throw("ray_intersect_gpu() should only be called in GPU mode.");
    }
}

MI_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test_gpu(const Ray3f &ray, Mask active) const {
    if constexpr (dr::is_cuda_v<Float>) {
#if defined(MI_OPTIX_ENABLE_BBOX_FASTPATH)
        if (m_use_bbox_fast_path) {
#else
        if (false) {
#endif
            // Attempt a fast path for scenes that are only made of a single AABB.
            SurfaceInteraction3f si = dr::zeros<SurfaceInteraction3f>();

            const ref<Shape> shape         = m_shapes[0];
            const ScalarBoundingBox3f bbox = shape->bbox();

            auto [hit, mint, maxt] = bbox.ray_intersect(ray);
            Mask starts_outside    = mint > 0.f;
            Float t                = dr::select(starts_outside, mint, maxt);
            return active && hit && (t <= ray.maxt) &&
                   (t > math::RayEpsilon<Float>);
        } else {
            OptixSceneState &s = *(OptixSceneState *) m_accel;
            const OptixConfig &config = optix_configs[s.config_index];

            // Override optix configuration in drjit-core.
            // TODO This could be problematic when raytracing calls on other scenes are still pending.
            jit_optix_configure(
                &config.pipeline_compile_options,
                &s.sbt,
                config.program_groups, ProgramGroupCount
            );

            UInt32 ray_mask(255),
                ray_flags(OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT |
                            OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT),
                sbt_offset(0), sbt_stride(1), miss_sbt_index(0);

            UInt32 payload_hit(1);

            using Single = dr::float32_array_t<Float>;
            dr::Array<Single, 3> ray_o(ray.o), ray_d(ray.d);
            Single ray_mint(0.f), ray_maxt(ray.maxt), ray_time(ray.time);

            // Be careful with 'ray.maxt' in double precision variants
            if constexpr (!std::is_same_v<Single, Float>)
                ray_maxt = dr::minimum(ray_maxt, dr::Largest<Single>);

            uint32_t trace_args[] {
                m_accel_handle.index(),
                ray_o.x().index(), ray_o.y().index(), ray_o.z().index(),
                ray_d.x().index(), ray_d.y().index(), ray_d.z().index(),
                ray_mint.index(), ray_maxt.index(), ray_time.index(),
                ray_mask.index(), ray_flags.index(),
                sbt_offset.index(), sbt_stride.index(),
                miss_sbt_index.index(), payload_hit.index()
            };

            jit_optix_ray_trace(sizeof(trace_args) / sizeof(uint32_t),
                                trace_args, active.index());

            return active && dr::eq(UInt32::steal(trace_args[15]), 1);
        }
    } else {
        DRJIT_MARK_USED(ray);
        DRJIT_MARK_USED(active);
        Throw("ray_test_gpu() should only be called in GPU mode.");
    }
}

NAMESPACE_END(mitsuba)
