#include <drjit/dynamic.h>
#include <drjit/tensor.h>
#include <drjit/texture.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/spectrum.h>
#include <mitsuba/core/string.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/render/srgb.h>
#include <mitsuba/render/volume.h>
#include <mitsuba/render/volumegrid.h>

// TODO: rewrite this to be more general and move to Dr.Jit
NAMESPACE_BEGIN(drjit)
template <typename T>
std::tuple<T, T, T> meshgrid(const T &x, const T &y, const T &z,
                             bool index_xy = true) {
    static_assert(array_depth_v<T> == 1 && is_dynamic_array_v<T>,
                  "meshgrid(): requires three 1D dynamic DrJit arrays as input!");

    uint32_t lx = (uint32_t) x.size(),
             ly = (uint32_t) y.size(),
             lz = (uint32_t) z.size();

    if (lx == 1 || ly == 1 || lz == 1) {
        // TODO: not sure if this case is correct
        return { x, y, z };
    } else {
        using UInt32 = uint32_array_t<T>;
        size_t total = lx * ly * lz;
        UInt32 index = arange<UInt32>(total);

        T inputs[3];
        inputs[0] = index_xy ? y : x;
        inputs[1] = index_xy ? x : y;
        inputs[2] = z;

        // TODO: possible to make this faster with `idivmod`?
        T result[3];
        for (size_t k = 0; k < 3; ++k) {
            total /= inputs[k].size();
            UInt32 index_v = index / total;
            index -= index_v * total;
            result[k] = gather<T>(inputs[k], index_v);
        }

        if (index_xy)
            return { result[1], result[0], result[2] };
        else
            return { result[0], result[1], result[2] };
    }
}
NAMESPACE_END(drjit)

NAMESPACE_BEGIN(mitsuba)

/**!
.. _volume-gridvolume:

Grid-based volume data source (:monosp:`gridvolume`)
----------------------------------------------------

.. pluginparameters::
 :extra-rows: 6

 * - filename
   - |string|
   - Filename of the volume to be loaded

 * - grid
   - :monosp:`VolumeGrid object`
   - When creating a grid volume at runtime, e.g. from Python or C++,
     an existing ``VolumeGrid`` instance can be passed directly rather than
     loading it from the filesystem with :paramtype:`filename`.

 * - filter_type
   - |string|
   - Specifies how voxel values are interpolated. The following options are
     currently available:

     - ``trilinear`` (default): perform trilinear interpolation.

     - ``nearest``: disable interpolation. In this mode, the plugin
       performs nearest neighbor lookups of volume values.

 * - wrap_mode
   - |string|
   - Controls the behavior of volume evaluations that fall outside of the
     :math:`[0, 1]` range. The following options are currently available:

     - ``clamp`` (default): clamp coordinates to the edge of the volume.

     - ``repeat``: tile the volume infinitely.

     - ``mirror``: mirror the volume along its boundaries.

 * - raw
   - |bool|
   - Should the transformation to the stored color data (e.g. sRGB to linear,
     spectral upsampling) be disabled? You will want to enable this when working
     with non-color, 3-channel volume data. Currently, no plugin needs this option
     to be set to true (Default: false)

 * - to_world
   - |transform|
   - Specifies an optional 4x4 transformation matrix that will be applied to volume coordinates.

 * - accel
   - |bool|
   - Hardware acceleration features can be used in CUDA mode. These features can
     cause small differences as hardware interpolation methods typically have a
     loss of precision (not exactly 32-bit arithmetic). (Default: true)

 * - data
   - |tensor|
   - Tensor array containing the grid data.
   - |exposed|, |differentiable|

This class implements access to volume data stored on a 3D grid using a
simple binary exchange format (compatible with Mitsuba 0.6). When appropriate,
spectral upsampling is applied at loading time to convert RGB values to
spectra that can be used in the renderer.
We provide a small `helper utility <https://github.com/mitsuba-renderer/mitsuba2-vdb-converter>`_
to convert OpenVDB files to this format. The format uses a
little endian encoding and is specified as follows:

.. list-table:: Volume file format
   :widths: 8 30
   :header-rows: 1

   * - Position
     - Content
   * - Bytes 1-3
     - ASCII Bytes ’V’, ’O’, and ’L’
   * - Byte 4
     - File format version number (currently 3)
   * - Bytes 5-8
     - Encoding identified (32-bit integer). Currently, only a value of 1 is
       supported (float32-based representation)
   * - Bytes 9-12
     - Number of cells along the X axis (32 bit integer)
   * - Bytes 13-16
     - Number of cells along the Y axis (32 bit integer)
   * - Bytes 17-20
     - Number of cells along the Z axis (32 bit integer)
   * - Bytes 21-24
     - Number of channels (32 bit integer, supported values: 1, 3 or 6)
   * - Bytes 25-48
     - Axis-aligned bounding box of the data stored in single precision (order:
       xmin, ymin, zmin, xmax, ymax, zmax)
   * - Bytes 49-*
     - Binary data of the volume stored in the specified encoding. The data
       are ordered so that the following C-style indexing operation makes sense
       after the file has been loaded into memory:
       :code:`data[((zpos*yres + ypos)*xres + xpos)*channels + chan]`
       where (xpos, ypos, zpos, chan) denotes the lookup location.

.. tabs::
    .. code-tab:: xml

        <medium type="heterogeneous">
            <volume type="grid" name="albedo">
                <string name="filename" value="my_volume.vol"/>
            </volume>
        </medium>

    .. code-tab:: python

        'type': 'heterogeneous',
        'albedo': {
            'type': 'grid',
            'filename': 'my_volume.vol'
        }

*/

/**
 * Interpolated 3D grid texture of scalar or color values.
 *
 * This plugin loads RGB data from a binary file. When appropriate,
 * spectral upsampling is applied at loading time to convert RGB values
 * to spectra that can be used in the renderer.
 *
 * Data layout:
 * The data must be ordered so that the following C-style (row-major) indexing
 * operation makes sense after the file has been mapped into memory:
 *     data[((zpos*yres + ypos)*xres + xpos)*channels + chan]}
 *     where (xpos, ypos, zpos, chan) denotes the lookup location.
 */
template <typename Float, typename Spectrum>
class GridVolume final : public Volume<Float, Spectrum> {
public:
    MI_IMPORT_BASE(Volume, update_bbox, m_to_local, m_bbox, m_channel_count)
    MI_IMPORT_TYPES(VolumeGrid)

    GridVolume(const Properties &props) : Base(props) {
        std::string filter_type_str = props.string("filter_type", "trilinear");
        dr::FilterMode filter_mode;
        if (filter_type_str == "nearest")
            filter_mode = dr::FilterMode::Nearest;
        else if (filter_type_str == "trilinear")
            filter_mode = dr::FilterMode::Linear;
        else
            Throw("Invalid filter type \"%s\", must be one of: \"nearest\" or "
                  "\"trilinear\"!", filter_type_str);

        std::string wrap_mode_st = props.string("wrap_mode", "clamp");
        dr::WrapMode wrap_mode;
        if (wrap_mode_st == "repeat")
            wrap_mode = dr::WrapMode::Repeat;
        else if (wrap_mode_st == "mirror")
            wrap_mode = dr::WrapMode::Mirror;
        else if (wrap_mode_st == "clamp")
            wrap_mode = dr::WrapMode::Clamp;
        else
            Throw("Invalid wrap mode \"%s\", must be one of: \"repeat\", "
                  "\"mirror\", or \"clamp\"!",
                  wrap_mode_st);

        if (props.has_property("grid")) {
            // Creates a texture directly from an existing object
            if (props.has_property("filename"))
                Throw("Cannot specify both \"grid\" and \"filename\".");
            Log(Debug, "Loading volume grid from memory...");
            // Note: ref-counted, so we don't have to worry about lifetime
            ref<Object> other = props.object("grid");
            VolumeGrid *volume_grid = dynamic_cast<VolumeGrid *>(other.get());
            if (!volume_grid)
                Throw("Property \"grid\" must be a VolumeGrid instance.");
            m_volume_grid = volume_grid;
        } else if (props.has_property("data")) {
            // --- Getting grid data directly from the properties
            // Note: we expect the tensor data to be directly given
            // with the correct data layout, i.e. (Z, Y, X, C).
            const void *ptr = props.pointer("data");
            const TensorXf *data = (TensorXf *) ptr;
            const auto &shape = data->shape();

            if (!props.get<bool>("raw", true))
                Throw("Passing grid data directly implies raw = true");
            if (props.get<bool>("use_grid_bbox", false))
                Throw("Passing grid data directly implies use_grid_bbox = false");
            if (data->ndim() != 4)
                Throw("The given \"data\" tensor must have 4 dimensions, found %s", data->ndim());

            // Initialize the rest of the fields from the given data
            // TODO: initialize it properly if it's really needed
            m_volume_grid = new VolumeGrid(
                ScalarVector3u(shape[2], shape[1], shape[0]), shape[3]);
            /* TODO: better syntax for this */ {
                Float tmp = dr::max(data->array());
                if constexpr (dr::is_jit_v<Float>)
                    m_volume_grid->set_max(tmp[0]);
                else
                    m_volume_grid->set_max(tmp);
            }

            // Copy data over from Tensor to VolumeGrid
            using HostFloat  = dr::DynamicArray<ScalarFloat>;
            using HostUInt32 = dr::uint32_array_t<HostFloat>;
            const HostFloat host_data = dr::migrate(data->array(), AllocType::Host);
            dr::sync_thread();
            dr::scatter(m_volume_grid->data(), host_data, dr::arange<HostUInt32>(host_data.size()));
            dr::eval(m_volume_grid->data());
            dr::sync_thread();
        } else {
            FileResolver *fs = Thread::thread()->file_resolver();
            fs::path file_path = fs->resolve(props.string("filename"));
            if (!fs::exists(file_path))
                Log(Error, "\"%s\": file does not exist!", file_path);
            m_volume_grid = new VolumeGrid(file_path);
        }

        m_raw = props.get<bool>("raw", false);

        m_accel = props.get<bool>("accel", true);

        ScalarVector3i res = m_volume_grid->size();
        ScalarUInt32 size = dr::prod(res);

        // Apply spectral conversion if necessary
        if (is_spectral_v<Spectrum> && m_volume_grid->channel_count() == 3 &&
            !m_raw) {
            ScalarFloat *ptr = m_volume_grid->data();

            auto scaled_data =
                std::unique_ptr<ScalarFloat[]>(new ScalarFloat[size * 4]);
            ScalarFloat *scaled_data_ptr = scaled_data.get();
            ScalarFloat max = 0.0;
            for (ScalarUInt32 i = 0; i < size; ++i) {
                ScalarColor3f rgb = dr::load<ScalarColor3f>(ptr);
                // TODO: Make this scaling optional if the RGB values are
                // between 0 and 1
                ScalarFloat scale = dr::max(rgb) * 2.f;
                ScalarColor3f rgb_norm =
                    rgb / dr::maximum((ScalarFloat) 1e-8, scale);
                ScalarVector3f coeff = srgb_model_fetch(rgb_norm);
                max = dr::maximum(max, scale);
                dr::store(scaled_data_ptr,
                          dr::concat(coeff, dr::Array<ScalarFloat, 1>(scale)));
                ptr += 3;
                scaled_data_ptr += 4;
            }

            size_t shape[4] = {
                (size_t) res.z(),
                (size_t) res.y(),
                (size_t) res.x(),
                4
            };
            m_texture = Texture3f(TensorXf(scaled_data.get(), 4, shape),
                                  m_accel, m_accel, filter_mode, wrap_mode);
        } else {
            size_t shape[4] = {
                (size_t) res.z(),
                (size_t) res.y(),
                (size_t) res.x(),
                m_volume_grid->channel_count()
            };
            m_texture = Texture3f(TensorXf(m_volume_grid->data(), 4, shape),
                                  m_accel, m_accel, filter_mode, wrap_mode);
            m_max = m_volume_grid->max();
            m_max_per_channel.resize(m_volume_grid->channel_count());
            m_volume_grid->max_per_channel(m_max_per_channel.data());
            m_channel_count = m_volume_grid->channel_count();
        }

        if (props.get<bool>("use_grid_bbox", false)) {
            m_to_local = m_volume_grid->bbox_transform() * m_to_local;
            update_bbox();
        }

        if (props.has_property("max_value")) {
            m_fixed_max = true;
            m_max       = props.get<ScalarFloat>("max_value");
            Throw("This feature should probably not be used until more correctness checks are in place");
        }

        // In the context of an optimization, we might want to keep the majorant
        // fixed to an initial value computed from the reference data, for convenience.
        if (props.has_property("fixed_max")) {
            m_fixed_max = props.get<bool>("fixed_max");
            // It's easy to get incorrect results by e.g.:
            // 1. Loading the scene with some data X with fixed_max = true
            // 2. From Python, overwriting the grid data with Y > X
            // 3. The medium majorant will not get updated, which is incorrect.
            if (m_fixed_max)
                Throw("This feature should probably not be used until more correctness checks are in place");
        }

        if (m_fixed_max)
            Log(Info, "Medium will keep majorant fixed to: %s", m_max);
        m_max_dirty = !dr::isfinite(m_max);
    }

    void traverse(TraversalCallback *callback) override {
        Base::traverse(callback);
        callback->put_parameter("data", m_texture.tensor(), +ParamFlags::Differentiable);
    }

    void parameters_changed(const std::vector<std::string> &keys) override {
        if (keys.empty() || string::contains(keys, "data")) {
            const size_t channels = nchannels();
            if (channels != 1 && channels != 3 && channels != 6)
                Throw("parameters_changed(): The volume data %s was changed "
                      "to have %d channels, only volumes with 1, 3 or 6 "
                      "channels are supported!", to_string(), channels);

            m_texture.set_tensor(m_texture.tensor());

            if (!m_fixed_max)
                m_max_dirty = true;
        }
    }

    UnpolarizedSpectrum eval(const Interaction3f &it,
                             Mask active) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);

        const size_t channels = nchannels();
        if (channels == 3 && is_spectral_v<Spectrum> && m_raw)
            Throw("The GridVolume texture %s was queried for a spectrum, but "
                  "texture conversion into spectra was explicitly disabled! "
                  "(raw=true)",
                  to_string());
        else if (channels != 3 && channels != 1)
            Throw("The GridVolume texture %s was queried for a spectrum, but "
                  "has a number of channels which is not 1 or 3",
                  to_string());
        else {
            if (dr::none_or<false>(active))
                return dr::zeros<UnpolarizedSpectrum>();

            if constexpr (is_monochromatic_v<Spectrum>) {
                if (channels == 1)
                    return interpolate_1(it, active);
                else // 3 channels
                    return luminance(interpolate_3(it, active));
            }
            else{
                if (channels == 1)
                    return interpolate_1(it, active);
                else { // 3 channels
                    if constexpr (is_spectral_v<Spectrum>)
                        return interpolate_spectral(it, active);
                    else
                        return interpolate_3(it, active);
                }
            }
        }
    }

    Float eval_1(const Interaction3f &it, Mask active = true) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);

        const size_t channels = nchannels();
        if (channels == 3 && is_spectral_v<Spectrum> && !m_raw)
            Throw("eval_1(): The GridVolume texture %s was queried for a "
                  "scalar value, but texture conversion into spectra was "
                  "requested! (raw=false)",
                  to_string());
        else {
            if (dr::none_or<false>(active))
                return dr::zeros<Float>();

            if (channels == 1)
                return interpolate_1(it, active);
            else if (channels == 3)
                return luminance(interpolate_3(it, active));
            else // 6 channels
                return dr::mean(interpolate_6(it, active));
        }
    }

    void eval_n(const Interaction3f &it, Float *out, Mask active = true) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);

        return interpolate_per_channel<Float>(it, out, active);
    }

    Vector3f eval_3(const Interaction3f &it,
                    Mask active = true) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);

        const size_t channels = nchannels();
        if (channels != 3) {
            Throw("eval_3(): The GridVolume texture %s was queried for a 3D "
                  "vector, but it has %s channel(s)", to_string(), channels);
        } else if (is_spectral_v<Spectrum> && !m_raw) {
            Throw("eval_3(): The GridVolume texture %s was queried for a 3D "
                  "vector, but texture conversion into spectra was requested! "
                  "(raw=false)", to_string());
        } else {
            if (dr::none_or<false>(active))
                return dr::zeros<Vector3f>();

            return interpolate_3(it, active);
        }
    }

    dr::Array<Float, 6> eval_6(const Interaction3f &it,
                               Mask active = true) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);

        const size_t channels = nchannels();
        if (channels != 6)
            Throw("eval_6(): The GridVolume texture %s was queried for a 6D "
                  "vector, but it has %s channel(s)", to_string(), channels);
        else {
            if (dr::none_or<false>(active))
                return dr::zeros<dr::Array<Float, 6>>();

            return interpolate_6(it, active);
        }
    }

    ScalarFloat max() const override {
        if (m_max_dirty)
            update_max();
        return m_max;
    }

    void update_max() const {
        if (m_max_dirty) {
            m_max = (float) dr::max_nested(dr::detach(m_texture.value()));
            Log(Info, "Grid max value was updated to: %s", m_max);
            m_max_dirty = false;
        }
    }

    void max_per_channel(ScalarFloat *out) const override {
        for (size_t i=0; i<m_max_per_channel.size(); ++i)
            out[i] = m_max_per_channel[i];
    }

    ScalarVector3i resolution() const override {
        const size_t *shape = m_texture.shape();
        return { (int) shape[2], (int) shape[1], (int) shape[0] };
    };

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "GridVolume[" << std::endl
            << "  to_local = " << string::indent(m_to_local, 13) << "," << std::endl
            << "  bbox = " << string::indent(m_bbox) << "," << std::endl
            << "  dimensions = " << resolution() << "," << std::endl
            << "  max = " << m_max << "," << std::endl
            << "  channels = " << m_texture.shape()[3] << std::endl
            << "]";
        return oss.str();
    }

    MI_DECLARE_CLASS()

protected:
    /**
     * \brief Returns the number of channels in the grid
     *
     * For object instances that perform spectral upsampling, the channel that
     * holds all scaling coefficients is omitted.
     */
    MI_INLINE size_t nchannels() const {
        const size_t channels = m_texture.shape()[3];
        // When spectral upsampling is requested, a fourth channel is added to
        // the internal texture data to handle scaling coefficients.
        if (is_spectral_v<Spectrum> && channels == 4 && !m_raw)
            return 3;

        return channels;
    }

    /**
     * \brief Evaluates the volume at the given interaction using spectral
     * upsampling
     */
    MI_INLINE UnpolarizedSpectrum interpolate_spectral(const Interaction3f &it,
                                                        Mask active) const {
        MI_MASK_ARGUMENT(active);

        Point3f p = m_to_local * it.p;

        if (m_texture.filter_mode() == dr::FilterMode::Linear) {
            dr::Array<Float, 4> d000, d100, d010, d110, d001, d101, d011, d111;
            dr::Array<Float *, 8> fetch_values;
            fetch_values[0] = d000.data();
            fetch_values[1] = d100.data();
            fetch_values[2] = d010.data();
            fetch_values[3] = d110.data();
            fetch_values[4] = d001.data();
            fetch_values[5] = d101.data();
            fetch_values[6] = d011.data();
            fetch_values[7] = d111.data();

            if (m_accel)
                m_texture.eval_fetch(p, fetch_values, active);
            else
                m_texture.eval_fetch_nonaccel(p, fetch_values, active);

            UnpolarizedSpectrum v000, v001, v010, v011, v100, v101, v110, v111;
            v000 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d000), it.wavelengths);
            v100 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d100), it.wavelengths);
            v010 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d010), it.wavelengths);
            v110 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d110), it.wavelengths);
            v001 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d001), it.wavelengths);
            v101 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d101), it.wavelengths);
            v011 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d011), it.wavelengths);
            v111 = srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(d111), it.wavelengths);

            ScalarVector3i res = resolution();
            p = dr::fmadd(p, res, -.5f);
            Vector3i p_i = dr::floor2int<Vector3i>(p);

            // Interpolation weights
            Point3f w1 = p - Point3f(p_i),
                    w0 = 1.f - w1;

            Float f00 = dr::fmadd(w0.x(), d000.w(), w1.x() * d100.w()),
                  f01 = dr::fmadd(w0.x(), d001.w(), w1.x() * d101.w()),
                  f10 = dr::fmadd(w0.x(), d010.w(), w1.x() * d110.w()),
                  f11 = dr::fmadd(w0.x(), d011.w(), w1.x() * d111.w());
            Float f0 = dr::fmadd(w0.y(), f00, w1.y() * f10),
                  f1 = dr::fmadd(w0.y(), f01, w1.y() * f11);
            Float scale = dr::fmadd(w0.z(), f0, w1.z() * f1);

            UnpolarizedSpectrum v00 = dr::fmadd(w0.x(), v000, w1.x() * v100),
                                v01 = dr::fmadd(w0.x(), v001, w1.x() * v101),
                                v10 = dr::fmadd(w0.x(), v010, w1.x() * v110),
                                v11 = dr::fmadd(w0.x(), v011, w1.x() * v111);
            UnpolarizedSpectrum v0 = dr::fmadd(w0.y(), v00, w1.y() * v10),
                                v1 = dr::fmadd(w0.y(), v01, w1.y() * v11);
            UnpolarizedSpectrum result = dr::fmadd(w0.z(), v0, w1.z() * v1);

            result *= scale;

            return result;
        } else {
            dr::Array<Float, 4> v;
            if (m_accel)
                m_texture.eval(p, v.data(), active);
            else
                m_texture.eval_nonaccel(p, v.data(), active);

            return v.w() * srgb_model_eval<UnpolarizedSpectrum>(dr::head<3>(v), it.wavelengths);
        }
    }

    TensorXf local_majorants(size_t resolution_factor, ScalarFloat value_scale) override {
        MI_IMPORT_TYPES()
        using Value   = mitsuba::DynamicBuffer<Float>;
        using Index   = mitsuba::DynamicBuffer<UInt32>;
        using Index3i = mitsuba::Vector<mitsuba::DynamicBuffer<Int32>, 3>;

        if (m_accel)
            NotImplementedError("local_majorants() with m_accel");

        if (m_texture.shape()[3] != 1)
            NotImplementedError("local_majorants() when Channels != 1");

        if constexpr (dr::is_jit_v<Float>) {
            dr::eval(m_texture.value());
            dr::sync_thread();
        }

        // This is the real (user-facing) resolution, but recall
        // that the layout in memory is (Z, Y, X, C).
        const ScalarVector3i full_resolution = resolution();
        if ((full_resolution.x() % resolution_factor) != 0 ||
            (full_resolution.y() % resolution_factor) != 0 ||
            (full_resolution.z() % resolution_factor) != 0) {
            Throw("Supergrid construction: grid resolution %s must be divisible by %s",
                full_resolution, resolution_factor);
        }
        const ScalarVector3i resolution(
            full_resolution.x() / resolution_factor,
            full_resolution.y() / resolution_factor,
            full_resolution.z() / resolution_factor
        );

        Log(Debug, "Constructing supergrid of resolution %s from full grid of resolution %s",
            resolution, full_resolution);
        size_t n = dr::prod(resolution);
        Value result = dr::full<Value>(-dr::Infinity<Float>, n);

        // Z is the slowest axis, X is the fastest.
        auto [Z, Y, X] = dr::meshgrid(
            dr::arange<Index>(resolution.z()), dr::arange<Index>(resolution.y()),
            dr::arange<Index>(resolution.x()), /*index_xy*/ false);
        Index3i cell_indices = resolution_factor * Index3i(X, Y, Z);

        // We have to include all values that participate in interpolated
        // lookups if we want the true maximum over a region.
        int32_t begin_offset, end_offset;
        switch (m_texture.filter_mode()) {
            case dr::FilterMode::Nearest: {
                begin_offset = 0;
                end_offset = resolution_factor;
                break;
            }
            case dr::FilterMode::Linear: {
                begin_offset = -1;
                end_offset = resolution_factor + 1;
                break;
            }
        };

        Value scaled_data = value_scale * dr::detach(m_texture.value());

        // TODO: any way to do this without the many operations?
        for (int32_t dz = begin_offset; dz < end_offset; ++dz) {
            for (int32_t dy = begin_offset; dy < end_offset; ++dy) {
                for (int32_t dx = begin_offset; dx < end_offset; ++dx) {
                    // Ensure valid lookups with clamping
                    Index3i offset_indices = Index3i(
                        dr::clamp(cell_indices.x() + dx, 0, full_resolution.x() - 1),
                        dr::clamp(cell_indices.y() + dy, 0, full_resolution.y() - 1),
                        dr::clamp(cell_indices.z() + dz, 0, full_resolution.z() - 1)
                    );
                    // Linearize indices
                    const Index idx =
                        offset_indices.x() +
                        offset_indices.y() * full_resolution.x() +
                        offset_indices.z() * full_resolution.x() *
                            full_resolution.y();

                    Value values =
                        dr::gather<Value>(scaled_data, idx);
                    result = dr::maximum(result, values);
                }
            }
        }

        size_t shape[4] = { (size_t) resolution.z(),
                            (size_t) resolution.y(),
                            (size_t) resolution.x(),
                            1 };
        return TensorXf(result, 4, shape);
    }

    /**
     * \brief Evaluates the volume at the given interaction
     *
     * Should only be used when the volume data has exactly 1 channel.
     */
    MI_INLINE Float interpolate_1(const Interaction3f &it, Mask active) const {
        MI_MASK_ARGUMENT(active);
        Point3f p = m_to_local * it.p;
        Float result;
        if (m_accel)
            m_texture.eval(p, &result, active);
        else
            m_texture.eval_nonaccel(p, &result, active);

        return result;
    }

    /**
     * \brief Evaluates the volume at the given interaction
     *
     * Should only be used when the volume data has exactly 3 channels.
     */
    MI_INLINE Color3f interpolate_3(const Interaction3f &it,
                                     Mask active) const {
        MI_MASK_ARGUMENT(active);

        Point3f p = m_to_local * it.p;
        Color3f result;
        if (m_accel)
            m_texture.eval(p, result.data(), active);
        else
            m_texture.eval_nonaccel(p, result.data(), active);

        return result;
    }

    /**
     * \brief Evaluates the volume at the given interaction
     *
     * Should only be used when the volume data has exactly 6 channels.
     */
    MI_INLINE dr::Array<Float, 6> interpolate_6(const Interaction3f &it,
                                                 Mask active) const {
        MI_MASK_ARGUMENT(active);

        Point3f p = m_to_local * it.p;
        dr::Array<Float, 6> result;
        if (m_accel)
            m_texture.eval(p, result.data(), active);
        else
            m_texture.eval_nonaccel(p, result.data(), active);

        return result;
    }


    /**
     * \brief Evaluates the volume at the given interaction
     *
     * Should only be used when the volume contains data with multiple channels
     */
    template <typename VALUE>
    MI_INLINE void interpolate_per_channel(const Interaction3f &it, Float *out, Mask active) const {
        MI_MASK_ARGUMENT(active);

        Point3f p = m_to_local * it.p;
        if (m_accel)
            m_texture.eval(p, out, active);
        else
            m_texture.eval_nonaccel(p, out, active);
    }

protected:
    Texture3f m_texture;
    bool m_accel;
    bool m_raw;
    ref<VolumeGrid> m_volume_grid;

    // Mutable so that we can do lazy updates
    mutable ScalarFloat m_max;
    mutable bool m_max_dirty;
    bool m_fixed_max = false;
    std::vector<ScalarFloat> m_max_per_channel;
};

MI_IMPLEMENT_CLASS_VARIANT(GridVolume, Volume)
MI_EXPORT_PLUGIN(GridVolume, "GridVolume texture")

NAMESPACE_END(mitsuba)
