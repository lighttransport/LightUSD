// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
emscripten::val RenderStream::getNode(int32_t node_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) {
      out.set("error", std::string("invalid node index"));
      return out;
    }
    const tr::SceneNode& node = render_scene_.nodes[static_cast<size_t>(node_id)];
    out.set("index", node_id);
    out.set("name", node.name);
    out.set("primPath", node.prim_path);
    out.set("type", NodeTypeName(node.type));
    out.set("visible", node.visible);
    out.set("dataId", node.data_id);
    out.set("parentId", node.parent_id);
    out.set("localMatrix", MatrixValue(MatrixToArray(node.local_transform)));
    out.set("worldMatrix", MatrixValue(MatrixToArray(node.world_transform)));
    out.set("children", VectorToArray(node.children));
    return out;
  }

emscripten::val RenderStream::getLight(int32_t light_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) {
      out.set("error", std::string("invalid light index"));
      return out;
    }
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    out.set("index", light_id);
    out.set("name", light.name);
    out.set("primPath", light.prim_path);
    out.set("type", LightTypeName(light.type));
    out.set("typeCode", static_cast<int>(light.type));
    out.set("intensity", light.intensity);
    out.set("exposure", light.exposure);
    out.set("normalize", light.normalize);
    out.set("enableColorTemperature", light.enable_color_temperature);
    out.set("colorTemperature", light.color_temperature);
    out.set("diffuse", light.diffuse);
    out.set("specular", light.specular);
    out.set("shapingFocus", light.shaping_focus);
    out.set("shapingFocusTint", Float3Value(light.shaping_focus_tint));
    out.set("shapingConeSoftness", light.shaping_cone_softness);
    out.set("shapingIesFile", light.shaping_ies_file);
    out.set("shapingIesAngleScale", light.shaping_ies_angle_scale);
    out.set("shapingIesNormalize", light.shaping_ies_normalize);
    out.set("lightLinkTargets", VectorToArray(light.light_link_targets));
    out.set("shadowLinkTargets", VectorToArray(light.shadow_link_targets));
    out.set("filterTargets", VectorToArray(light.filter_targets));
    // Resolved CollectionAPI membership: when *LinksAll is false, the
    // *LinkMeshIndices arrays list the affected RenderScene mesh ids.
    out.set("lightLinksAll", light.light_links_all);
    out.set("lightLinkMeshIndices", VectorToArray(light.light_link_mesh_indices));
    out.set("shadowLinksAll", light.shadow_links_all);
    out.set("shadowLinkMeshIndices", VectorToArray(light.shadow_link_mesh_indices));
    out.set("enableShadow", light.enable_shadow);
    out.set("color", Float3Value(light.color));
    out.set("transform", MatrixValue(MatrixToArray(light.transform)));
    out.set("shadowColor", Float3Value(light.shadow_color));
    out.set("shadowDistance", light.shadow_distance);
    out.set("shadowFalloff", light.shadow_falloff);
    out.set("shadowFalloffGamma", light.shadow_falloff_gamma);
    switch (light.type) {
      case tr::LightType::Sphere:
        out.set("radius", light.params.sphere.radius);
        break;
      case tr::LightType::Rect:
        out.set("width", light.params.rect.width);
        out.set("height", light.params.rect.height);
        break;
      case tr::LightType::Disk:
        out.set("radius", light.params.disk.radius);
        break;
      case tr::LightType::Spot:
        out.set("angle", light.params.spot.angle);
        break;
      case tr::LightType::Dome: {
        out.set("textureId", light.params.dome.texture_id);
        // Legacy light consumers use textureFile/envmapTextureId. The next
        // scene keeps dome images in RenderScene::images rather than the
        // legacy decoded-image table, so expose the authored path and let the
        // browser archive adapter resolve it (or recognize color_RRGGBB.exr).
        out.set("envmapTextureId", -1);
        if (light.params.dome.texture_id >= 0 &&
            static_cast<size_t>(light.params.dome.texture_id) <
                render_scene_.images.size()) {
          const tr::TextureImage& image = render_scene_.images[
              static_cast<size_t>(light.params.dome.texture_id)];
          out.set("textureFile", image.name.empty() ? image.resolved_path
                                                    : image.name);
        }
        const char* format = "automatic";
        switch (light.params.dome.texture_format) {
          case tr::RenderLight::DomeTextureFormat::Latlong:
            format = "latlong"; break;
          case tr::RenderLight::DomeTextureFormat::MirroredBall:
            format = "mirroredBall"; break;
          case tr::RenderLight::DomeTextureFormat::Angular:
            format = "angular"; break;
          default: break;
        }
        out.set("domeTextureFormat", std::string(format));
        break;
      }
      case tr::LightType::Cylinder:
        out.set("radius", light.params.cylinder.radius);
        out.set("length", light.params.cylinder.length);
        break;
      case tr::LightType::Directional:
        out.set("angle", light.params.distant.angle);
        break;
      default:
        break;
    }
    return out;
  }

emscripten::val RenderStream::getPoints(int32_t points_id) {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || points_id < 0 ||
        static_cast<size_t>(points_id) >= render_scene_.points.size()) {
      out.set("error", std::string("invalid points index"));
      return out;
    }
    const tr::RenderPoints& points =
        render_scene_.points[static_cast<size_t>(points_id)];
    s_points_cloud_points_.clear();
    s_points_cloud_widths_.clear();
    s_points_cloud_colors_.clear();
    s_points_cloud_points_.reserve(points.points.size());
    for (size_t i = 0; i < points.points.size(); ++i) {
      s_points_cloud_points_.push_back(points.points[i]);
    }
    s_points_cloud_widths_.reserve(points.widths.size());
    for (size_t i = 0; i < points.widths.size(); ++i) {
      s_points_cloud_widths_.push_back(points.widths[i]);
    }
    s_points_cloud_colors_.reserve(points.colors.size());
    for (size_t i = 0; i < points.colors.size(); ++i) {
      s_points_cloud_colors_.push_back(points.colors[i]);
    }

    out.set("index", points_id);
    out.set("name", points.name);
    out.set("primPath", points.prim_path);
    out.set("pointCount", static_cast<int>(points.point_count()));
    out.set("materialId", points.material_id);
    out.set("points", heapF_(s_points_cloud_points_, 3));
    if (!s_points_cloud_widths_.empty()) {
      out.set("widths", heapF_(s_points_cloud_widths_, 1));
    }
    if (!s_points_cloud_colors_.empty()) {
      out.set("colors", heapF_(s_points_cloud_colors_, 3));
    }
    out.set("hasBounds", points.has_bbox);
    if (points.has_bbox) {
      out.set("bboxMin", Float3Value(points.bbox_min));
      out.set("bboxMax", Float3Value(points.bbox_max));
    }
    return out;
  }

emscripten::val RenderStream::getCurves(int32_t curves_id) {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || curves_id < 0 ||
        static_cast<size_t>(curves_id) >= render_scene_.curves.size()) {
      out.set("error", std::string("invalid curves index"));
      return out;
    }
    const tr::RenderCurves& curves =
        render_scene_.curves[static_cast<size_t>(curves_id)];

    s_curve_points_.clear();
    s_curve_widths_.clear();
    s_curve_colors_.clear();
    s_curve_tessellated_points_.clear();
    s_curve_tessellated_widths_.clear();
    s_curve_tessellated_colors_.clear();
    s_curve_points_.reserve(curves.points.size());
    for (size_t i = 0; i < curves.points.size(); ++i) {
      s_curve_points_.push_back(curves.points[i]);
    }
    s_curve_widths_.reserve(curves.widths.size());
    for (size_t i = 0; i < curves.widths.size(); ++i) {
      s_curve_widths_.push_back(curves.widths[i]);
    }
    s_curve_colors_.reserve(curves.colors.size());
    for (size_t i = 0; i < curves.colors.size(); ++i) {
      s_curve_colors_.push_back(curves.colors[i]);
    }
    s_curve_tessellated_points_.reserve(curves.tessellated_points.size());
    for (size_t i = 0; i < curves.tessellated_points.size(); ++i) {
      s_curve_tessellated_points_.push_back(curves.tessellated_points[i]);
    }
    s_curve_tessellated_widths_.reserve(curves.tessellated_widths.size());
    for (size_t i = 0; i < curves.tessellated_widths.size(); ++i) {
      s_curve_tessellated_widths_.push_back(curves.tessellated_widths[i]);
    }
    s_curve_tessellated_colors_.reserve(curves.tessellated_colors.size());
    for (size_t i = 0; i < curves.tessellated_colors.size(); ++i) {
      s_curve_tessellated_colors_.push_back(curves.tessellated_colors[i]);
    }

    out.set("index", curves_id);
    out.set("name", curves.name);
    out.set("primPath", curves.prim_path);
    out.set("curveCount", static_cast<int>(curves.curve_count()));
    out.set("controlPointCount", static_cast<int>(curves.control_point_count()));
    out.set("tessellatedPointCount",
            static_cast<int>(curves.tessellated_point_count()));
    out.set("type", CurveTypeName(curves.type));
    out.set("typeCode", static_cast<int>(curves.type));
    out.set("basis", CurveBasisName(curves.basis));
    out.set("basisCode", static_cast<int>(curves.basis));
    out.set("wrap", CurveWrapName(curves.wrap));
    out.set("wrapCode", static_cast<int>(curves.wrap));
    out.set("isNurbs", curves.is_nurbs);
    out.set("materialId", curves.material_id);
    out.set("widthsInterpolation", InterpolationName(curves.widths_interp));
    out.set("colorsInterpolation", InterpolationName(curves.colors_interp));
    out.set("curveVertexCounts", VectorToArray(curves.curve_vertex_counts));
    out.set("tessellatedVertexCounts",
            VectorToArray(curves.tessellated_vertex_counts));
    out.set("points", heapF_(s_curve_points_, 3));
    out.set("tessellatedPoints", heapF_(s_curve_tessellated_points_, 3));
    if (!s_curve_widths_.empty()) {
      out.set("widths", heapF_(s_curve_widths_, 1));
    }
    if (!s_curve_colors_.empty()) {
      out.set("colors", heapF_(s_curve_colors_, 3));
    }
    if (!s_curve_tessellated_widths_.empty()) {
      out.set("tessellatedWidths", heapF_(s_curve_tessellated_widths_, 1));
    }
    if (!s_curve_tessellated_colors_.empty()) {
      out.set("tessellatedColors", heapF_(s_curve_tessellated_colors_, 3));
    }
    out.set("hasBounds", curves.has_bbox);
    if (curves.has_bbox) {
      out.set("bboxMin", Float3Value(curves.bbox_min));
      out.set("bboxMax", Float3Value(curves.bbox_max));
    }
    return out;
  }

emscripten::val RenderStream::getCamera(int32_t camera_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) {
      out.set("error", std::string("invalid camera index"));
      return out;
    }
    const tr::RenderCamera& camera = render_scene_.cameras[static_cast<size_t>(camera_id)];
    out.set("index", camera_id);
    out.set("name", camera.name);
    out.set("primPath", camera.prim_path);
    out.set("type", CameraTypeName(camera.type));
    out.set("typeCode", static_cast<int>(camera.type));
    out.set("transform", MatrixValue(MatrixToArray(camera.transform)));
    out.set("focalLength", camera.focal_length);
    out.set("horizontalAperture", camera.horizontal_aperture);
    out.set("verticalAperture", camera.vertical_aperture);
    out.set("orthoWidth", camera.ortho_width);
    out.set("nearClip", camera.near_clip);
    out.set("farClip", camera.far_clip);
    out.set("focusDistance", camera.focus_distance);
    out.set("fStop", camera.fstop);
    out.set("shutterOpen", camera.shutter_open);
    out.set("shutterClose", camera.shutter_close);
    out.set("fovY", camera.fov_y());
    out.set("fovX", camera.fov_x());
    out.set("aspect", camera.aspect_ratio());
    return out;
  }

emscripten::val RenderStream::getPointInstancer(int32_t instancer_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || instancer_id < 0 ||
        static_cast<size_t>(instancer_id) >=
            render_scene_.point_instancers.size()) {
      out.set("error", std::string("invalid point instancer index"));
      return out;
    }
    const tr::RenderPointInstancer& instancer =
        render_scene_.point_instancers[static_cast<size_t>(instancer_id)];
    out.set("index", instancer_id);
    out.set("name", instancer.name);
    out.set("primPath", instancer.prim_path);
    out.set("prototypePaths", VectorToArray(instancer.prototype_paths));
    out.set("prototypeNodeIds", VectorToArray(instancer.prototype_node_ids));
    out.set("prototypeMeshOffsets", VectorToArray(instancer.prototype_mesh_offsets));
    out.set("prototypeMeshIds", VectorToArray(instancer.prototype_mesh_ids));
    out.set("drawStart", static_cast<int>(instancer.draw_start));
    out.set("drawCount", static_cast<int>(instancer.draw_count));
    out.set("protoCount", static_cast<int>(instancer.prototype_count()));
    out.set("instanceCount", static_cast<int>(instancer.instance_count()));
    out.set("visibleInstanceCount",
            static_cast<int>(instancer.visible_instance_count()));
    out.set("hasTransforms", !instancer.transforms.empty());
    out.set("hasOrientations", instancer.has_orientations());
    out.set("hasScales", instancer.has_scales());
    out.set("hasVelocities", instancer.has_velocities());
    out.set("hasAngularVelocities", instancer.has_angular_velocities());
    out.set("valid", instancer.valid);
    if (!instancer.validation_error.empty()) {
      out.set("validationError", instancer.validation_error);
    }
    return out;
  }

emscripten::val RenderStream::getPointInstanceDraw(int32_t draw_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || draw_id < 0 ||
        static_cast<size_t>(draw_id) >=
            render_scene_.point_instance_draws.size()) {
      out.set("error", std::string("invalid point instance draw index"));
      return out;
    }
    const tr::RenderPointInstanceDraw* draw =
        render_scene_.get_point_instance_draw(static_cast<size_t>(draw_id));
    if (!draw) {
      out.set("error", std::string("invalid point instance draw index"));
      return out;
    }
    out.set("index", draw_id);
    out.set("pointInstancerId", draw->point_instancer_id);
    out.set("instanceIndex", static_cast<int>(draw->instance_index));
    out.set("prototypeIndex", static_cast<int>(draw->prototype_index));
    out.set("meshId", draw->mesh_id);
    out.set("materialId", draw->material_id);
    out.set("expandedMeshId", draw->expanded_mesh_id);
    out.set("transform", MatrixValue(MatrixToArray(draw->transform)));
    if (draw->mesh_id >= 0 &&
        static_cast<size_t>(draw->mesh_id) < render_scene_.meshes.size()) {
      out.set("meshPath", render_scene_.meshes[static_cast<size_t>(draw->mesh_id)].prim_path);
    }
    if (draw->material_id >= 0 &&
        static_cast<size_t>(draw->material_id) < render_scene_.materials.size()) {
      out.set("materialPath", render_scene_.materials[static_cast<size_t>(draw->material_id)].prim_path);
    }
    return out;
  }

emscripten::val RenderStream::getSkeleton(int32_t skeleton_id) const {
    emscripten::val out = emscripten::val::object();
    if (!render_scene_valid_ || skeleton_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size()) {
      out.set("error", std::string("invalid skeleton index"));
      return out;
    }
    const tr::Skeleton& skel = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    out.set("index", skeleton_id);
    out.set("name", skel.name);
    out.set("primPath", skel.prim_path);
    out.set("rootJoint", skel.root_joint);
    out.set("jointCount", static_cast<int>(skel.joints.size()));
    out.set("animationId", skel.animation_id);
    out.set("animationSourcePath", skel.animation_source_path);
    emscripten::val joints = emscripten::val::array();
    for (size_t i = 0; i < skel.joints.size(); ++i) {
      const tr::SkeletonJoint& j = skel.joints[i];
      emscripten::val jo = emscripten::val::object();
      jo.set("index", static_cast<int>(i));
      jo.set("name", j.name);
      jo.set("path", j.path);
      jo.set("parentId", j.parent_id);
      jo.set("bindMatrix", MatrixValue(MatrixToArray(j.bind_transform)));
      jo.set("restMatrix", MatrixValue(MatrixToArray(j.rest_transform)));
      jo.set("children", VectorToArray(j.children));
      joints.set(static_cast<int>(i), jo);
    }
    out.set("joints", joints);
    return out;
  }
}  // namespace web_next
}  // namespace lightusd
