#pragma once
// A settings store: a base value (a quality tier's preset, a struct's defaults, what the window
// was at startup) plus per-knob overrides that survive a rebase. The resolved value is a pure
// function of the two: base, then each override, then the table's clamp, then the struct's
// cross-field normalize. Pure: no godot-cpp.
// Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.2.
#include "settings/settings_table.h"
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ve {

// What VoxelSettings and the debug hooks see: a group of named knobs, type-erased.
class SettingsGroup {
public:
	virtual ~SettingsGroup() = default;
	virtual const std::vector<RowInfo> &rows() const = 0;
	virtual bool get(const char *name, SettingValue *out) const = 0;
	// The resolved base, ignoring overrides: what clearing this knob would give.
	virtual bool get_default(const char *name, SettingValue *out) const = 0;
	// False for an unknown name or a value of the wrong kind; nothing is recorded then.
	virtual bool set(const char *name, const SettingValue &v) = 0;
	// In row order; each value is the clamped value that was set.
	virtual std::vector<std::pair<const char *, SettingValue>> overrides() const = 0;
	virtual bool clear(const char *name) = 0;
	virtual void clear_all() = 0;
};

template <class T>
class SettingsStore : public SettingsGroup {
public:
	using Rows = std::span<const SettingRow<T>>;
	using Normalize = void (*)(T *);
	using Listener = void (*)(const T &resolved, void *ctx);

	SettingsStore(Rows rows, Normalize normalize, const T &base) :
			rows_(rows), normalize_(normalize), overrides_(rows.size()), base_(base) {
		infos_.reserve(rows_.size());
		for (const SettingRow<T> &r : rows_) infos_.push_back(row_info(r));
		resolved_ = resolve(base_, overrides_);
	}

	// Wiring, before any concurrent use. Called after every mutation with the new resolved value,
	// on the mutating thread, OUTSIDE this store's lock -- so a listener may read this store or
	// mutate another one.
	void set_listener(Listener fn, void *ctx) {
		listener_ = fn;
		listener_ctx_ = ctx;
	}

	// Replace the base and drop every override.
	void set(const T &base) {
		mutate([&] {
			base_ = base;
			for (std::optional<SettingValue> &o : overrides_) o.reset();
		});
	}
	// Replace the base and keep the overrides (a tier change).
	void rebase(const T &base) {
		mutate([&] { base_ = base; });
	}
	T get() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return resolved_;
	}
	T base() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return base_;
	}
	// Name-addressed convenience for bool, int, float and enum rows; the float is read as the
	// row's own kind. False for an unknown name or a colour row.
	bool set_value(const char *name, float v) {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || r->kind == SettingKind::kColor) return false;
		SettingValue sv;
		sv.kind = r->kind;
		sv.v[0] = v;
		return set(name, sv);
	}
	float value(const char *name) const {
		SettingValue v;
		return get(name, &v) ? v.v[0] : 0.0f;
	}

	const std::vector<RowInfo> &rows() const override { return infos_; }

	bool get(const char *name, SettingValue *out) const override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || !out) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		*out = read(*r, resolved_);
		return true;
	}

	bool get_default(const char *name, SettingValue *out) const override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || !out) return false;
		const T def = resolve(base(), std::vector<std::optional<SettingValue>>(rows_.size()));
		*out = read(*r, def);
		return true;
	}

	bool set(const char *name, const SettingValue &v) override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || v.kind != r->kind) return false;
		// Stored clamped, so overrides() reports the value that actually took.
		T scratch{};
		write(*r, &scratch, v);
		const SettingValue clamped = read(*r, scratch);
		const size_t i = static_cast<size_t>(r - rows_.data());
		mutate([&] { overrides_[i] = clamped; });
		return true;
	}

	std::vector<std::pair<const char *, SettingValue>> overrides() const override {
		std::vector<std::pair<const char *, SettingValue>> out;
		std::lock_guard<std::mutex> lock(mutex_);
		for (size_t i = 0; i < rows_.size(); i++)
			if (overrides_[i]) out.emplace_back(rows_[i].name, *overrides_[i]);
		return out;
	}

	bool clear(const char *name) override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r) return false;
		const size_t i = static_cast<size_t>(r - rows_.data());
		mutate([&] { overrides_[i].reset(); });
		return true;
	}

	void clear_all() override {
		mutate([&] {
			for (std::optional<SettingValue> &o : overrides_) o.reset();
		});
	}

private:
	template <class F>
	void mutate(F &&change) {
		T resolved;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			change();
			resolved_ = resolve(base_, overrides_);
			resolved = resolved_;
		}
		if (listener_) listener_(resolved, listener_ctx_);
	}

	T resolve(const T &base, const std::vector<std::optional<SettingValue>> &overrides) const {
		T out = base;
		for (size_t i = 0; i < rows_.size(); i++)
			if (overrides[i]) write(rows_[i], &out, *overrides[i]);
		clamp_all(rows_, &out);
		if (normalize_) normalize_(&out);
		return out;
	}

	Rows rows_;
	Normalize normalize_ = nullptr;
	std::vector<RowInfo> infos_;
	Listener listener_ = nullptr;
	void *listener_ctx_ = nullptr;
	mutable std::mutex mutex_;
	std::vector<std::optional<SettingValue>> overrides_;
	T base_;
	T resolved_;
};

} // namespace ve
