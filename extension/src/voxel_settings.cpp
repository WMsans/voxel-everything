#include "voxel_settings.h"
#include "render/orchestrator.h"
#include "settings/godot/setting_variant.h"
#include "voxel_world.h"
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <algorithm>
#include <iterator>
#include <utility>

using namespace godot;

namespace {

// Panel tab order.
constexpr const char *kGroups[] = {"display", "render", "beauty", "grass"};

// In the order of the display row's upscaler options (settings/display_settings.cpp).
const Viewport::Scaling3DMode kUpscalerModes[] = {
		Viewport::SCALING_3D_MODE_BILINEAR,
		Viewport::SCALING_3D_MODE_FSR,
		Viewport::SCALING_3D_MODE_FSR2,
		Viewport::SCALING_3D_MODE_METALFX_SPATIAL,
		Viewport::SCALING_3D_MODE_METALFX_TEMPORAL,
};
static_assert(std::size(kUpscalerModes) == ve::kUpscalerOptionCount);

int upscaler_index_of(Viewport::Scaling3DMode mode) {
	for (int i = 0; i < static_cast<int>(std::size(kUpscalerModes)); i++)
		if (kUpscalerModes[i] == mode) return i;
	return 0;
}

bool is_editor() {
	return Engine::get_singleton()->is_editor_hint();
}

bool is_fullscreen(DisplayServer *ds) {
	const DisplayServer::WindowMode m = ds->window_get_mode();
	return m == DisplayServer::WINDOW_MODE_FULLSCREEN ||
			m == DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN;
}

} // namespace

void VoxelSettings::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_path", "p"), &VoxelSettings::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &VoxelSettings::get_world_path);
	ClassDB::bind_method(D_METHOD("set_viewport_path", "p"), &VoxelSettings::set_viewport_path);
	ClassDB::bind_method(D_METHOD("get_viewport_path"), &VoxelSettings::get_viewport_path);
	ClassDB::bind_method(D_METHOD("set_config_path", "p"), &VoxelSettings::set_config_path);
	ClassDB::bind_method(D_METHOD("get_config_path"), &VoxelSettings::get_config_path);
	ClassDB::bind_method(D_METHOD("set_manage_window", "v"), &VoxelSettings::set_manage_window);
	ClassDB::bind_method(D_METHOD("get_manage_window"), &VoxelSettings::get_manage_window);
	ClassDB::bind_method(D_METHOD("groups"), &VoxelSettings::groups);
	ClassDB::bind_method(D_METHOD("describe", "group"), &VoxelSettings::describe);
	ClassDB::bind_method(D_METHOD("get_setting", "group", "name"), &VoxelSettings::get_setting);
	ClassDB::bind_method(D_METHOD("set_setting", "group", "name", "value"), &VoxelSettings::set_setting);
	ClassDB::bind_method(D_METHOD("get_overrides", "group"), &VoxelSettings::get_overrides);
	ClassDB::bind_method(D_METHOD("clear_overrides", "group"), &VoxelSettings::clear_overrides);
	ClassDB::bind_method(D_METHOD("apply_config", "args"), &VoxelSettings::apply_config);
	ClassDB::bind_method(D_METHOD("save"), &VoxelSettings::save);
	ClassDB::bind_method(D_METHOD("reset_to_shipped"), &VoxelSettings::reset_to_shipped);
	ClassDB::bind_method(D_METHOD("resolution_index_of", "size"), &VoxelSettings::resolution_index_of);
	ClassDB::bind_static_method("VoxelSettings", D_METHOD("is_measured_args", "args"),
			&VoxelSettings::is_measured_args);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_path"), "set_world_path", "get_world_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "viewport_path"), "set_viewport_path",
			"get_viewport_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "config_path"), "set_config_path", "get_config_path");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "manage_window"), "set_manage_window",
			"get_manage_window");
}

VoxelSettings::VoxelSettings() {
	display_.set_listener(&VoxelSettings::on_display_resolved, this);
}

ve::SettingsGroup *VoxelSettings::group(const String &name) const {
	if (name == "display") return &display_;
	if (VoxelWorld *world = Object::cast_to<VoxelWorld>(ObjectDB::get_instance(world_id_))) {
		const CharString n = name.utf8();
		return world->context().render->settings_group(n.get_data());
	}
	// ponytail: the beauty stand-in is not rebased when the render stand-in's tier changes, so the
	// editor reverts beauty knobs to High's values. Wire a listener if a scene ever ships a tier.
	if (name == "render") return &render_stand_in_;
	if (name == "beauty") return &beauty_stand_in_;
	if (name == "grass") return &grass_stand_in_;
	return nullptr;
}

void VoxelSettings::_ready() {
	// The editor edits property values; it never applies them to a world, a viewport, a window or
	// a file. Not resolving the world there also keeps the inspector on this node's own values.
	if (is_editor()) return;
	if (!world_path_.is_empty())
		if (VoxelWorld *world = Object::cast_to<VoxelWorld>(get_node_or_null(world_path_)))
			world_id_ = world->get_instance_id();
	Viewport *vp = viewport_path_.is_empty() ? get_viewport()
											 : Object::cast_to<Viewport>(get_node_or_null(viewport_path_));
	viewport_id_ = vp ? vp->get_instance_id() : 0;
	// A scene's property values went into the stand-ins before the world was known.
	if (world_id_ != 0) {
		const std::pair<const char *, ve::SettingsGroup *> stand_ins[] = {
			{"render", &render_stand_in_}, {"beauty", &beauty_stand_in_}, {"grass", &grass_stand_in_}};
		for (const auto &[name, stand_in] : stand_ins)
			if (ve::SettingsGroup *live = group(name))
				for (const auto &[knob, value] : stand_in->overrides()) live->set(knob, value);
	}
	capture_display_base();
	ready_ = true;
	apply_display(display_.get());
	for (const char *g : kGroups) shipped_[g] = get_overrides(g);
	apply_config(OS::get_singleton()->get_cmdline_user_args());
}

void VoxelSettings::capture_display_base() {
	ve::DisplaySettings base;
	if (Viewport *vp = Object::cast_to<Viewport>(ObjectDB::get_instance(viewport_id_))) {
		base.render_scale = vp->get_scaling_3d_scale();
		base.upscaler = upscaler_index_of(vp->get_scaling_3d_mode());
	}
	if (manage_window_) {
		if (DisplayServer *ds = DisplayServer::get_singleton()) {
			const Vector2i size = ds->window_get_size();
			base.resolution = ve::resolution_index_of(size.x, size.y);
			base.fullscreen = is_fullscreen(ds);
		}
	}
	// Rebase, not set: overrides a scene applied before this node was ready survive.
	display_.rebase(base);
}

void VoxelSettings::on_display_resolved(const ve::DisplaySettings &s, void *ctx) {
	auto *self = static_cast<VoxelSettings *>(ctx);
	if (self->ready_) self->apply_display(s);
}

void VoxelSettings::apply_display(const ve::DisplaySettings &s) {
	if (Viewport *vp = Object::cast_to<Viewport>(ObjectDB::get_instance(viewport_id_))) {
		if (vp->get_scaling_3d_scale() != s.render_scale) vp->set_scaling_3d_scale(s.render_scale);
		const int last = static_cast<int>(std::size(kUpscalerModes)) - 1;
		const Viewport::Scaling3DMode mode = kUpscalerModes[std::clamp(s.upscaler, 0, last)];
		if (vp->get_scaling_3d_mode() != mode) vp->set_scaling_3d_mode(mode);
	}
	if (!manage_window_) return;
	DisplayServer *ds = DisplayServer::get_singleton();
	if (!ds) return;
	if (s.fullscreen != is_fullscreen(ds))
		ds->window_set_mode(s.fullscreen ? DisplayServer::WINDOW_MODE_FULLSCREEN
										 : DisplayServer::WINDOW_MODE_WINDOWED);
	// ponytail: a resolution picked while fullscreen waits until fullscreen is turned off; the old
	// F7 popup left fullscreen for you. Add that back if anyone misses it.
	if (s.fullscreen || s.resolution < 0) return;
	const ve::WindowSize &want = ve::kResolutions[s.resolution];
	const Vector2i size(want.w, want.h);
	// Not applied when it already matches: booting should not churn the window.
	if (ds->window_get_size() != size) ds->window_set_size(size);
}

PackedStringArray VoxelSettings::groups() const {
	PackedStringArray out;
	for (const char *g : kGroups) out.push_back(String(g));
	return out;
}

Array VoxelSettings::describe(const String &group_name) const {
	Array out;
	ve::SettingsGroup *g = group(group_name);
	if (!g) return out;
	for (const ve::RowInfo &r : g->rows()) {
		Dictionary d;
		d["name"] = String(r.name);
		d["label"] = String(r.label);
		d["hint"] = r.hint ? String(r.hint) : String();
		d["kind"] = setting_kind_name(r.kind);
		d["min"] = r.min;
		d["max"] = r.max;
		d["ui_min"] = r.ui_min;
		d["ui_max"] = r.ui_max;
		d["step"] = r.step;
		PackedStringArray options;
		for (const char *o : r.options) options.push_back(String(o));
		d["options"] = options;
		ve::SettingValue v;
		if (g->get(r.name, &v)) d["value"] = setting_to_variant(v);
		if (g->get_default(r.name, &v)) d["default"] = setting_to_variant(v);
		out.push_back(d);
	}
	return out;
}

Variant VoxelSettings::get_setting(const String &group_name, const String &name) const {
	ve::SettingsGroup *g = group(group_name);
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!g || !g->get(n.get_data(), &v)) return Variant();
	return setting_to_variant(v);
}

bool VoxelSettings::set_setting(const String &group_name, const String &name, const Variant &value) {
	ve::SettingsGroup *g = group(group_name);
	const CharString n = name.utf8();
	ve::SettingValue current;
	if (!g || !g->get(n.get_data(), &current)) return false;
	ve::SettingValue v;
	if (!setting_from_variant(current.kind, value, &v)) return false;
	return g->set(n.get_data(), v);
}

Dictionary VoxelSettings::get_overrides(const String &group_name) const {
	Dictionary d;
	if (ve::SettingsGroup *g = group(group_name))
		for (const auto &[name, value] : g->overrides()) d[String(name)] = setting_to_variant(value);
	return d;
}

void VoxelSettings::clear_overrides(const String &group_name) {
	if (ve::SettingsGroup *g = group(group_name)) g->clear_all();
}

bool VoxelSettings::is_measured_args(const PackedStringArray &args) {
	for (int i = 0; i < args.size(); i++)
		if (args[i].begins_with("--benchmark") || args[i] == "--capture") return true;
	return false;
}

bool VoxelSettings::apply_config(const PackedStringArray &args) {
	measured_ = is_measured_args(args);
	if (measured_ || is_editor()) return false;
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	if (cfg->load(config_path_) != OK) return false;
	// Overrides are sticky, so the order the sections apply in does not matter: a tier loaded
	// after a beauty knob rebases beauty without dropping it.
	for (const char *g : kGroups) {
		if (!cfg->has_section(g)) continue;
		const PackedStringArray keys = cfg->get_section_keys(g);
		for (int i = 0; i < keys.size(); i++) set_setting(g, keys[i], cfg->get_value(g, keys[i]));
	}
	return true;
}

bool VoxelSettings::save() {
	if (measured_ || is_editor()) return false;
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	cfg->load(config_path_); // keep sections this node does not own
	for (const char *g : kGroups) {
		if (cfg->has_section(g)) cfg->erase_section(g);
		const Dictionary overrides = get_overrides(g);
		const Array keys = overrides.keys();
		for (int i = 0; i < keys.size(); i++)
			cfg->set_value(g, String(keys[i]), overrides[keys[i]]);
	}
	return cfg->save(config_path_) == OK;
}

void VoxelSettings::reset_to_shipped() {
	for (const char *g : kGroups) {
		clear_overrides(g);
		const Dictionary shipped = shipped_.get(g, Dictionary());
		const Array keys = shipped.keys();
		for (int i = 0; i < keys.size(); i++) set_setting(g, String(keys[i]), shipped[keys[i]]);
	}
}

int VoxelSettings::resolution_index_of(Vector2i size) const {
	return ve::resolution_index_of(size.x, size.y);
}

namespace {

bool split_property(const StringName &property, String *group, String *name) {
	const String p = property;
	const int slash = p.find("/");
	if (slash <= 0) return false;
	*group = p.substr(0, slash);
	*name = p.substr(slash + 1);
	return true;
}

} // namespace

bool VoxelSettings::_set(const StringName &property, const Variant &value) {
	String g, n;
	return split_property(property, &g, &n) && set_setting(g, n, value);
}

bool VoxelSettings::_get(const StringName &property, Variant &r_value) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	if (!grp || !grp->get(name.get_data(), &v)) return false;
	r_value = setting_to_variant(v);
	return true;
}

void VoxelSettings::_get_property_list(List<PropertyInfo> *list) const {
	for (const char *g : kGroups) {
		ve::SettingsGroup *grp = group(g);
		if (!grp) continue;
		for (const ve::RowInfo &r : grp->rows()) {
			const String path = String(g) + "/" + r.name;
			const String range = String::num(r.ui_min) + "," + String::num(r.ui_max) + "," +
					String::num(r.step);
			switch (r.kind) {
				case ve::SettingKind::kBool:
					list->push_back(PropertyInfo(Variant::BOOL, path));
					break;
				case ve::SettingKind::kInt:
					list->push_back(PropertyInfo(Variant::INT, path, PROPERTY_HINT_RANGE, range));
					break;
				case ve::SettingKind::kFloat:
					list->push_back(PropertyInfo(Variant::FLOAT, path, PROPERTY_HINT_RANGE, range));
					break;
				case ve::SettingKind::kColor:
					list->push_back(PropertyInfo(Variant::COLOR, path, PROPERTY_HINT_COLOR_NO_ALPHA));
					break;
				case ve::SettingKind::kEnum: {
					String items = r.min < 0.0f ? String("Not a preset:-1") : String();
					for (size_t i = 0; i < r.options.size(); i++) {
						if (!items.is_empty()) items += ",";
						items += String(r.options[i]) + ":" + String::num_int64(static_cast<int64_t>(i));
					}
					list->push_back(PropertyInfo(Variant::INT, path, PROPERTY_HINT_ENUM, items));
					break;
				}
			}
		}
	}
}

bool VoxelSettings::_property_can_revert(const StringName &property) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	return grp && grp->get_default(name.get_data(), &v);
}

bool VoxelSettings::_property_get_revert(const StringName &property, Variant &r_value) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	if (!grp || !grp->get_default(name.get_data(), &v)) return false;
	r_value = setting_to_variant(v);
	return true;
}
