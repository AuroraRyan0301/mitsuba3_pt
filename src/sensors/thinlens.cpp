#include <mitsuba/render/sensor.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/core/bbox.h>
#include <mitsuba/core/warp.h>

NAMESPACE_BEGIN(mitsuba)

/**!

.. _sensor-thinlens:

Perspective camera with a thin lens (:monosp:`thinlens`)
--------------------------------------------------------

.. pluginparameters::
 :extra-rows: 8

 * - to_world
   - |transform|
   - Specifies an optional camera-to-world transformation.
     (Default: none (i.e. camera space = world space))
   - |exposed|

 * - aperture_radius
   - |float|
   - Denotes the radius of the camera's aperture in scene units.
   - |exposed|

 * - focus_distance
   - |float|
   - Denotes the world-space distance from the camera's aperture to the focal plane.
     (Default: :monosp:`0`)
   - |exposed|

 * - focal_length
   - |string|
   - Denotes the camera's focal length specified using *35mm* film equivalent units.
     See the main description for further details. (Default: :monosp:`50mm`)

 * - fov
   - |float|
   - An alternative to :monosp:`focal_length`: denotes the camera's field of view in degrees---must be
     between 0 and 180, excluding the extremes.

 * - fov_axis
   - |string|
   - When the parameter :monosp:`fov` is given (and only then), this parameter further specifies
     the image axis, to which it applies.

     1. :monosp:`x`: :monosp:`fov` maps to the :monosp:`x`-axis in screen space.
     2. :monosp:`y`: :monosp:`fov` maps to the :monosp:`y`-axis in screen space.
     3. :monosp:`diagonal`: :monosp:`fov` maps to the screen diagonal.
     4. :monosp:`smaller`: :monosp:`fov` maps to the smaller dimension
        (e.g. :monosp:`x` when :monosp:`width` < :monosp:`height`)
     5. :monosp:`larger`: :monosp:`fov` maps to the larger dimension
        (e.g. :monosp:`y` when :monosp:`width` < :monosp:`height`)

     The default is :monosp:`x`.

 * - near_clip, far_clip
   - |float|
   - Distance to the near/far clip planes. (Default: :monosp:`near_clip=1e-2` (i.e. :monosp:`0.01`)
     and :monosp:`far_clip=1e4` (i.e. :monosp:`10000`))
   - |exposed|

 * - srf
   - |spectrum|
   - Sensor Response Function that defines the :ref:`spectral sensitivity <explanation_srf_sensor>`
     of the sensor (Default: :monosp:`none`)

 * - x_fov
   - |float|
   - Denotes the camera's field of view in degrees along the horizontal axis.
   - |exposed|

.. subfigstart::
.. subfigure:: ../../resources/data/docs/images/render/sensor_thinlens_small_aperture.jpg
   :caption: The material test ball viewed through a perspective thin lens camera. (:monosp:`aperture_radius=0.1`)
.. subfigure:: ../../resources/data/docs/images/render/sensor_thinlens.jpg
   :caption: The material test ball viewed through a perspective thin lens camera. (:monosp:`aperture_radius=0.2`)
.. subfigend::
   :label: fig-thinlens

This plugin implements a simple perspective camera model with a thin lens
at its circular aperture. It is very similar to the
:ref:`perspective <sensor-perspective>` plugin except that the extra lens element
permits rendering with a specifiable (i.e. non-infinite) depth of field.
To configure this, it has two extra parameters named :monosp:`aperture_radius`
and :monosp:`focus_distance`.

By default, the camera's field of view is specified using a 35mm film
equivalent focal length, which is first converted into a diagonal field
of view and subsequently applied to the camera. This assumes that
the film's aspect ratio matches that of 35mm film (1.5:1), though the
parameter still behaves intuitively when this is not the case.
Alternatively, it is also possible to specify a field of view in degrees
along a given axis (see the :monosp:`fov` and :monosp:`fov_axis` parameters).

The exact camera position and orientation is most easily expressed using the
:monosp:`lookat` tag, i.e.:

.. tabs::
    .. code-tab:: xml

        <sensor type="thinlens">
            <float name="fov" value="45"/>
            <transform name="to_world">
                <!-- Move and rotate the camera so that looks from (1, 1, 1) to (1, 2, 1)
                    and the direction (0, 0, 1) points "up" in the output image -->
                <lookat origin="1, 1, 1" target="1, 2, 1" up="0, 0, 1"/>
            </transform>

            <!-- Focus on the target -->
            <float name="focus_distance" value="1"/>
            <float name="aperture_radius" value="0.1"/>

            <!-- film -->
            <!-- sampler -->
        </sensor>

    .. code-tab:: python

        'type'='thinlens',
        'fov': 45,
        'to_world': mi.ScalarTransform4f.lookat(
            origin=[1, 1, 1],
            target=[1, 2, 1],
            up=[0, 0, 1]
        ),
        'focus_distance': 1.0,
        'aperture_radius': 0.1,
        'film_id': {
            'type': '<film_type>',
            # ...
        },
        'sampler_id': {
            'type': '<sampler_type>',
            # ...
        }

 */

template <typename Float, typename Spectrum>
class ThinLensCamera final : public ProjectiveCamera<Float, Spectrum> {
public:
    MI_IMPORT_BASE(ProjectiveCamera, m_to_world, m_needs_sample_3, m_film, m_sampler,
                    m_resolution, m_shutter_open, m_shutter_open_time, m_near_clip,
                    m_far_clip, m_focus_distance, sample_wavelengths)
    MI_IMPORT_TYPES()

    ThinLensCamera(const Properties &props) : Base(props) {
        ScalarVector2i size = m_film->size();
        m_x_fov = (ScalarFloat) parse_fov(props, size.x() / (double) size.y());

        m_aperture_radius = props.get<ScalarFloat>("aperture_radius");

        if (m_aperture_radius == 0.f) {
            Log(Warn, "Can't have a zero aperture radius -- setting to %f", dr::Epsilon<Float>);
            m_aperture_radius = dr::Epsilon<Float>;
        }

        if (m_to_world.scalar().has_scale())
            Throw("Scale factors in the camera-to-world transformation are not allowed!");

        update_camera_transforms();

        m_needs_sample_3 = true;
    }

    void traverse(TraversalCallback *callback) override {
        Base::traverse(callback);
        callback->put_parameter("aperture_radius", m_aperture_radius, +ParamFlags::NonDifferentiable);
        callback->put_parameter("focus_distance", m_focus_distance,   +ParamFlags::NonDifferentiable);
        callback->put_parameter("x_fov", m_x_fov,                     +ParamFlags::NonDifferentiable);
        callback->put_parameter("to_world", *m_to_world.ptr(),        +ParamFlags::NonDifferentiable);
    }

    void parameters_changed(const std::vector<std::string> &keys) override {
        if (keys.empty() || string::contains(keys, "to_world")) {
            // Update the scalar value of the matrix
            m_to_world = m_to_world.value();
            if (m_to_world.scalar().has_scale())
                Throw("Scale factors in the camera-to-world transformation are not allowed!");
        }
        Base::parameters_changed(keys);
        update_camera_transforms();
    }

    void update_camera_transforms() {
        m_camera_to_sample = perspective_projection(
            m_film->size(), m_film->crop_size(), m_film->crop_offset(),
            m_x_fov, m_near_clip, m_far_clip);

        m_sample_to_camera = m_camera_to_sample.inverse();

        // Position differentials on the near plane
        m_dx = m_sample_to_camera * ScalarPoint3f(1.f / m_resolution.x(), 0.f, 0.f)
             - m_sample_to_camera * ScalarPoint3f(0.f);
        m_dy = m_sample_to_camera * ScalarPoint3f(0.f, 1.f / m_resolution.y(), 0.f)
             - m_sample_to_camera * ScalarPoint3f(0.f);

        /* Precompute some data for importance(). Please
           look at that function for further details. */
        ScalarPoint3f pmin(m_sample_to_camera * ScalarPoint3f(0.f, 0.f, 0.f)),
                      pmax(m_sample_to_camera * ScalarPoint3f(1.f, 1.f, 0.f));

        m_image_rect.reset();
        m_image_rect.expand(ScalarPoint2f(pmin.x(), pmin.y()) / pmin.z());
        m_image_rect.expand(ScalarPoint2f(pmax.x(), pmax.y()) / pmax.z());
        m_normalization = 1.f / m_image_rect.volume();
    }

    std::pair<Ray3f, Spectrum> sample_ray(Float time, Float wavelength_sample,
                                          const Point2f &position_sample,
                                          const Point2f &aperture_sample,
                                          Mask active) const override {
        MI_MASKED_FUNCTION(ProfilerPhase::EndpointSampleRay, active);

        auto [wavelengths, wav_weight] =
            sample_wavelengths(dr::zeros<SurfaceInteraction3f>(),
                               wavelength_sample,
                               active);
        Ray3f ray;
        ray.time = time;
        ray.wavelengths = wavelengths;

        // Compute the sample position on the near plane (local camera space).
        Point3f near_p = m_sample_to_camera *
                        Point3f(position_sample.x(), position_sample.y(), 0.f);

        // Aperture position
        Point2f tmp = m_aperture_radius * warp::square_to_uniform_disk_concentric(aperture_sample);
        Point3f aperture_p(tmp.x(), tmp.y(), 0.f);

        // Sampled position on the focal plane
        Point3f focus_p = near_p * (m_focus_distance / near_p.z());

        // Convert into a normalized ray direction; adjust the ray interval accordingly.
        Vector3f d = dr::normalize(Vector3f(focus_p - aperture_p));

        ray.o = m_to_world.value().transform_affine(aperture_p);
        ray.d = m_to_world.value() * d;

        Float inv_z = dr::rcp(d.z());
        Float near_t = m_near_clip * inv_z,
              far_t  = m_far_clip * inv_z;
        ray.o += ray.d * near_t;
        ray.maxt = far_t - near_t;

        return { ray, wav_weight };
    }

    std::pair<RayDifferential3f, Spectrum>
    sample_ray_differential_impl(Float time, Float wavelength_sample,
                                 const Point2f &position_sample, const Point2f &aperture_sample,
                                 Mask active) const {
        MI_MASKED_FUNCTION(ProfilerPhase::EndpointSampleRay, active);

        auto [wavelengths, wav_weight] =
            sample_wavelengths(dr::zeros<SurfaceInteraction3f>(),
                               wavelength_sample,
                               active);
        RayDifferential3f ray;
        ray.time = time;
        ray.wavelengths = wavelengths;

        // Compute the sample position on the near plane (local camera space).
        Point3f near_p = m_sample_to_camera *
                        Point3f(position_sample.x(), position_sample.y(), 0.f);

        // Aperture position
        Point2f tmp = m_aperture_radius * warp::square_to_uniform_disk_concentric(aperture_sample);
        Point3f aperture_p(tmp.x(), tmp.y(), 0.f);

        // Sampled position on the focal plane
        Float f_dist = m_focus_distance / near_p.z();
        Point3f focus_p   = near_p          * f_dist,
                focus_p_x = (near_p + m_dx) * f_dist,
                focus_p_y = (near_p + m_dy) * f_dist;

        // Convert into a normalized ray direction; adjust the ray interval accordingly.
        Vector3f d = dr::normalize(Vector3f(focus_p - aperture_p));

        ray.o = m_to_world.value().transform_affine(aperture_p);
        ray.d = m_to_world.value() * d;

        Float inv_z = dr::rcp(d.z());
        Float near_t = m_near_clip * inv_z,
              far_t  = m_far_clip * inv_z;
        ray.o += ray.d * near_t;
        ray.maxt = far_t - near_t;

        ray.o_x = ray.o_y = ray.o;

        ray.d_x = m_to_world.value() * dr::normalize(Vector3f(focus_p_x - aperture_p));
        ray.d_y = m_to_world.value() * dr::normalize(Vector3f(focus_p_y - aperture_p));
        ray.has_differentials = true;

        return { ray, wav_weight };
    }

    ScalarBoundingBox3f bbox() const override {
        ScalarPoint3f p = m_to_world.scalar() * ScalarPoint3f(0.f);
        return ScalarBoundingBox3f(p, p);
    }

    std::string to_string() const override {
        using string::indent;

        std::ostringstream oss;
        oss << "ThinLensCamera[" << std::endl
            << "  x_fov = " << m_x_fov << "," << std::endl
            << "  near_clip = " << m_near_clip << "," << std::endl
            << "  far_clip = " << m_far_clip << "," << std::endl
            << "  focus_distance = " << m_focus_distance << "," << std::endl
            << "  film = " << indent(m_film) << "," << std::endl
            << "  sampler = " << indent(m_sampler) << "," << std::endl
            << "  resolution = " << m_resolution << "," << std::endl
            << "  shutter_open = " << m_shutter_open << "," << std::endl
            << "  shutter_open_time = " << m_shutter_open_time << "," << std::endl
            << "  to_world = " << indent(m_to_world)  << std::endl
            << "]";
        return oss.str();
    }

    MI_DECLARE_CLASS()
private:
    ScalarTransform4f m_camera_to_sample;
    ScalarTransform4f m_sample_to_camera;
    ScalarBoundingBox2f m_image_rect;
    ScalarFloat m_aperture_radius;
    ScalarFloat m_normalization;
    ScalarFloat m_x_fov;
    ScalarVector3f m_dx, m_dy;
};

MI_IMPLEMENT_CLASS_VARIANT(ThinLensCamera, ProjectiveCamera)
MI_EXPORT_PLUGIN(ThinLensCamera, "Thin Lens Camera");
NAMESPACE_END(mitsuba)
