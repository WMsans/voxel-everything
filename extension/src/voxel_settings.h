#pragma once
// VoxelSettings -- the one owner of "what has been set". Every dial the demo exposes lives in a
// settings group: display (owned here: the Viewport's scaling and the window) and render, beauty,
// grass (RenderOrchestrator's, reached through VoxelWorld). This node adds what a group cannot do
// by itself: access by name from GDScript, describe() for the settings panel, ConfigFile
// persistence behind the measured-run guard, and inspector properties. VoxelWorld does not know
// this class exists. Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.5.
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <cstdint>
#include "grass/grass_settings_store.h"
#include "settings/display_settings.h"
#include "settings/render_settings.h"
#include "shade/beauty_settings_store.h"

namespace godot {

class VoxelSettings : public Node {
	GDCLASS(VoxelSettings, Node)

public:
	VoxelSettings();

	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	// Empty: the viewport this node is in.
	void set_viewport_path(const NodePath &p) { viewport_path_ = p; }
	NodePath get_viewport_path() const { return viewport_path_; }
	void set_config_path(const String &p) { config_path_ = p; }
	String get_config_path() const { return config_path_; }
	// False keeps the resolution and fullscreen rows away from DisplayServer (every test).
	void set_manage_window(bool v) { manage_window_ = v; }
	bool get_manage_window() const { return manage_window_; }

	PackedStringArray groups() const;
	Array describe(const String &group) const;
	Variant get_setting(const String &group, const String &name) const;
	// False for an unknown group or name, or a value that cannot be read as the row's kind.
	bool set_setting(const String &group, const String &name, const Variant &value);
	Dictionary get_overrides(const String &group) const;
	void clear_overrides(const String &group);
	// Loads config_path unless `args` name a measured run, and remembers that verdict for save().
	bool apply_config(const PackedStringArray &args);
	// Writes each group's overrides as one section. False in a measured run or in the editor.
	bool save();
	// Clears every override and re-applies what the scene had set when this node became ready.
	void reset_to_shipped();
	int resolution_index_of(Vector2i size) const;
	static bool is_measured_args(const PackedStringArray &args);

	void _ready() override;

protected:
	static void _bind_methods();

private:
	ve::SettingsGroup *group(const String &name) const;
	void capture_display_base();
	void apply_display(const ve::DisplaySettings &s);
	static void on_display_resolved(const ve::DisplaySettings &s, void *ctx);

	NodePath world_path_;
	NodePath viewport_path_;
	String config_path_ = "user://settings.cfg";
	bool manage_window_ = true;
	// ObjectIDs, not pointers: the world or the viewport may be freed before this node.
	uint64_t world_id_ = 0;
	uint64_t viewport_id_ = 0;
	bool ready_ = false;
	bool measured_ = false;
	Dictionary shipped_; // group -> overrides when this node became ready
	mutable ve::DisplaySettingsStore display_;
	// Until _ready resolves the world -- and always in the editor -- the world's groups are these
	// stand-ins, so a scene's property values have somewhere to land; _ready copies their overrides
	// into the world's stores.
	mutable ve::RenderSettingsStore render_stand_in_;
	mutable ve::BeautySettingsStore beauty_stand_in_;
	mutable ve::GrassSettingsStore grass_stand_in_;
};

} // namespace godot
