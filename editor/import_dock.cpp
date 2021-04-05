/*************************************************************************/
/*  import_dock.cpp                                                      */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2021 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2021 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "import_dock.h"
#include "editor_node.h"
#include "editor_resource_preview.h"

class ImportDockParameters : public Object {
	GDCLASS(ImportDockParameters, Object);

public:
	Map<StringName, Variant> values;
	List<PropertyInfo> properties;
	Ref<ResourceImporter> importer;
	Vector<String> paths;
	Set<StringName> checked;
	bool checking;

	bool _set(const StringName &p_name, const Variant &p_value) {
		if (values.has(p_name)) {
			values[p_name] = p_value;
			if (checking) {
				checked.insert(p_name);
				_change_notify();
			}
			return true;
		}

		return false;
	}

	bool _get(const StringName &p_name, Variant &r_ret) const {
		if (values.has(p_name)) {
			r_ret = values[p_name];
			return true;
		}

		return false;
	}
	void _get_property_list(List<PropertyInfo> *p_list) const {
		for (const List<PropertyInfo>::Element *E = properties.front(); E; E = E->next()) {
			if (!importer->get_option_visibility(E->get().name, values))
				continue;
			PropertyInfo pi = E->get();
			if (checking) {
				pi.usage |= PROPERTY_USAGE_CHECKABLE;
				if (checked.has(E->get().name)) {
					pi.usage |= PROPERTY_USAGE_CHECKED;
				}
			}
			p_list->push_back(pi);
		}
	}

	void update() {
		_change_notify();
	}

	ImportDockParameters() {
		checking = false;
	}
};

Ref<ConfigFile> ImportDock::_get_config(const String &p_path) {
	const String config_path = p_path + ".import";
	Ref<ConfigFile> config;
	if (config_cache.has(config_path)) {
		config = config_cache[config_path];
	} else {
		config.instance();
		Error err = config->load(config_path);
		if (err == OK) {
			config_cache[config_path] = config;
		} else {
			config.unref();
		}
	}

	return config;
}

void ImportDock::set_edit_path(const String &p_path) {
	clear();

	files_to_import.clear();
	files_to_import.push_back(p_path);
	if (!is_visible()) {
		return;
	}

	Ref<ConfigFile> config = _get_config(p_path);
	if (config.is_null()) {
		clear();
		return;
	}

	String importer_name = config->get_value("remap", "importer");

	params->importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(importer_name);

	params->paths.clear();
	params->paths.push_back(p_path);

	_update_options(config);

	List<Ref<ResourceImporter> > importers;
	ResourceFormatImporter::get_singleton()->get_importers_for_extension(p_path.get_extension(), &importers);
	List<Pair<String, String> > importer_names;

	for (List<Ref<ResourceImporter> >::Element *E = importers.front(); E; E = E->next()) {
		importer_names.push_back(Pair<String, String>(E->get()->get_visible_name(), E->get()->get_importer_name()));
	}

	importer_names.sort_custom<PairSort<String, String> >();

	import_as->clear();

	for (List<Pair<String, String> >::Element *E = importer_names.front(); E; E = E->next()) {
		import_as->add_item(E->get().first);
		import_as->set_item_metadata(import_as->get_item_count() - 1, E->get().second);
		if (E->get().second == importer_name) {
			import_as->select(import_as->get_item_count() - 1);
		}
	}

	import_as->add_separator();
	import_as->add_item(TTR("Keep File (No Import)"));
	import_as->set_item_metadata(import_as->get_item_count() - 1, "keep");
	if (importer_name == "keep") {
		import_as->select(import_as->get_item_count() - 1);
	}

	import->set_disabled(false);
	import_as->set_disabled(false);
	preset->set_disabled(false);

	imported->set_text(p_path.get_file());
}

void ImportDock::_update_options(const Ref<ConfigFile> &p_config) {
	List<ResourceImporter::ImportOption> options;

	if (params->importer.is_valid()) {
		params->importer->get_import_options(&options);
	}

	params->properties.clear();
	params->values.clear();
	params->checking = params->paths.size() > 1;
	params->checked.clear();

	for (List<ResourceImporter::ImportOption>::Element *E = options.front(); E; E = E->next()) {
		params->properties.push_back(E->get().option);
		if (p_config.is_valid() && p_config->has_section_key("params", E->get().option.name)) {
			params->values[E->get().option.name] = p_config->get_value("params", E->get().option.name);
		} else {
			params->values[E->get().option.name] = E->get().default_value;
		}
	}

	params->update();
	_update_preset_menu();
}

void ImportDock::set_edit_multiple_paths(const Vector<String> &p_paths) {
	clear();

	files_to_import.clear();
	files_to_import.append_array(p_paths);

	if (!is_visible()) {
		return;
	}

	importing_canceled = false;
	threadvar_importing_complete = false;
	thread.start(_thread_func, this);
}

void ImportDock::_thread_func(void *user_data) {
	ImportDock *id = (ImportDock *)user_data;
	id->_parse_config_files();
}

void ImportDock::_parse_config_files() {
	value_frequency.clear();

	for (int i = 0; i < files_to_import.size(); i++) {
		importing_mutex.lock();
		threadvar_importing_text = vformat(TTR("Processing %d / %d"), i, files_to_import.size());
		bool should_stop = importing_canceled;
		importing_mutex.unlock();
		if (should_stop) {
			return;
		}

		Ref<ConfigFile> config = _get_config(files_to_import[i]);
		ERR_CONTINUE(config.is_null());

		if (i == 0) {
			params->importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(config->get_value("remap", "importer"));
			if (params->importer.is_null()) {
				clear();
				return;
			}
		}

		if (!config->has_section("params")) {
			continue;
		}

		List<String> keys;
		config->get_section_keys("params", &keys);

		for (List<String>::Element *E = keys.front(); E; E = E->next()) {
			if (!value_frequency.has(E->get())) {
				value_frequency[E->get()] = Dictionary();
			}

			Variant value = config->get_value("params", E->get());

			if (value_frequency[E->get()].has(value)) {
				value_frequency[E->get()][value] = int(value_frequency[E->get()][value]) + 1;
			} else {
				value_frequency[E->get()][value] = 1;
			}
		}

		call_deferred("_finish_set_edit_multiple_paths");
	}

	importing_mutex.lock();
	threadvar_importing_complete = true;
	importing_mutex.unlock();

	call_deferred("_finish_set_edit_multiple_paths");
}

void ImportDock::_finish_set_edit_multiple_paths() {
	importing_mutex.lock();
	String label_text = threadvar_importing_text;
	bool complete = threadvar_importing_complete;
	importing_mutex.unlock();

	if (!importing_canceled) {
		imported->set_text(label_text);
	}

	if (!complete) {
		return;
	}

	ERR_FAIL_COND(params->importer.is_null());

	List<ResourceImporter::ImportOption> options;
	params->importer->get_import_options(&options);

	params->properties.clear();
	params->values.clear();
	params->checking = true;
	params->checked.clear();

	for (List<ResourceImporter::ImportOption>::Element *E = options.front(); E; E = E->next()) {
		params->properties.push_back(E->get().option);

		if (value_frequency.has(E->get().option.name)) {
			Dictionary d = value_frequency[E->get().option.name];
			int freq = 0;
			List<Variant> v;
			d.get_key_list(&v);
			Variant value;
			for (List<Variant>::Element *F = v.front(); F; F = F->next()) {
				int f = d[F->get()];
				if (f > freq) {
					value = F->get();
				}
			}

			params->values[E->get().option.name] = value;
		} else {
			params->values[E->get().option.name] = E->get().default_value;
		}
	}

	params->update();

	List<Ref<ResourceImporter> > importers;
	ResourceFormatImporter::get_singleton()->get_importers_for_extension(files_to_import[0].get_extension(), &importers);
	List<Pair<String, String> > importer_names;

	for (List<Ref<ResourceImporter> >::Element *E = importers.front(); E; E = E->next()) {
		importer_names.push_back(Pair<String, String>(E->get()->get_visible_name(), E->get()->get_importer_name()));
	}

	importer_names.sort_custom<PairSort<String, String> >();

	import_as->clear();

	for (List<Pair<String, String> >::Element *E = importer_names.front(); E; E = E->next()) {
		import_as->add_item(E->get().first);
		import_as->set_item_metadata(import_as->get_item_count() - 1, E->get().second);
		if (E->get().second == params->importer->get_importer_name()) {
			import_as->select(import_as->get_item_count() - 1);
		}
	}

	_update_preset_menu();

	params->paths = files_to_import;
	import->set_disabled(false);
	import_as->set_disabled(false);
	preset->set_disabled(false);

	imported->set_text(vformat(TTR("%d Files"), files_to_import.size()));
}

void ImportDock::_update_preset_menu() {
	preset->get_popup()->clear();

	if (params->importer.is_null()) {
		preset->get_popup()->add_item(TTR("Default"));
		preset->hide();
		return;
	}
	preset->show();

	if (params->importer->get_preset_count() == 0) {
		preset->get_popup()->add_item(TTR("Default"));
	} else {
		for (int i = 0; i < params->importer->get_preset_count(); i++) {
			preset->get_popup()->add_item(params->importer->get_preset_name(i));
		}
	}

	preset->get_popup()->add_separator();
	preset->get_popup()->add_item(vformat(TTR("Set as Default for '%s'"), params->importer->get_visible_name()), ITEM_SET_AS_DEFAULT);
	if (ProjectSettings::get_singleton()->has_setting("importer_defaults/" + params->importer->get_importer_name())) {
		preset->get_popup()->add_item(TTR("Load Default"), ITEM_LOAD_DEFAULT);
		preset->get_popup()->add_separator();
		preset->get_popup()->add_item(vformat(TTR("Clear Default for '%s'"), params->importer->get_visible_name()), ITEM_CLEAR_DEFAULT);
	}
}

void ImportDock::_importer_selected(int i_idx) {
	String name = import_as->get_selected_metadata();
	if (name == "keep") {
		params->importer.unref();
		_update_options(Ref<ConfigFile>());
	} else {
		Ref<ResourceImporter> importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(name);
		ERR_FAIL_COND(importer.is_null());

		params->importer = importer;

		Ref<ConfigFile> config;
		if (params->paths.size()) {
			config = _get_config(params->paths[0]);
		}
		_update_options(config);
	}
}

void ImportDock::_preset_selected(int p_idx) {
	int item_id = preset->get_popup()->get_item_id(p_idx);

	switch (item_id) {
		case ITEM_SET_AS_DEFAULT: {
			Dictionary d;

			for (const List<PropertyInfo>::Element *E = params->properties.front(); E; E = E->next()) {
				d[E->get().name] = params->values[E->get().name];
			}

			ProjectSettings::get_singleton()->set("importer_defaults/" + params->importer->get_importer_name(), d);
			ProjectSettings::get_singleton()->save();
			_update_preset_menu();
		} break;
		case ITEM_LOAD_DEFAULT: {
			ERR_FAIL_COND(!ProjectSettings::get_singleton()->has_setting("importer_defaults/" + params->importer->get_importer_name()));

			Dictionary d = ProjectSettings::get_singleton()->get("importer_defaults/" + params->importer->get_importer_name());
			List<Variant> v;
			d.get_key_list(&v);

			if (params->checking) {
				params->checked.clear();
			}
			for (List<Variant>::Element *E = v.front(); E; E = E->next()) {
				params->values[E->get()] = d[E->get()];
				if (params->checking) {
					params->checked.insert(E->get());
				}
			}
			params->update();
		} break;
		case ITEM_CLEAR_DEFAULT: {
			ProjectSettings::get_singleton()->set("importer_defaults/" + params->importer->get_importer_name(), Variant());
			ProjectSettings::get_singleton()->save();
			_update_preset_menu();
		} break;
		default: {
			List<ResourceImporter::ImportOption> options;

			params->importer->get_import_options(&options, p_idx);

			if (params->checking) {
				params->checked.clear();
			}
			for (List<ResourceImporter::ImportOption>::Element *E = options.front(); E; E = E->next()) {
				params->values[E->get().option.name] = E->get().default_value;
				if (params->checking) {
					params->checked.insert(E->get().option.name);
				}
			}
			params->update();
		} break;
	}
}

void ImportDock::files_removed(const Vector<String> &p_paths) {
	for (int i = 0; i < p_paths.size(); i++) {
		config_cache.erase(p_paths[i] + ".import");
	}
}

void ImportDock::clear() {
	if (thread.is_started()) {
		importing_mutex.lock();
		importing_canceled = true;
		importing_mutex.unlock();
		thread.wait_to_finish();
	}

	imported->set_text("");
	import->set_disabled(true);
	import_as->clear();
	import_as->set_disabled(true);
	preset->set_disabled(true);
	params->values.clear();
	params->properties.clear();
	params->update();
	preset->get_popup()->clear();
}

static bool _find_owners(EditorFileSystemDirectory *efsd, const String &p_path) {
	if (!efsd)
		return false;

	for (int i = 0; i < efsd->get_subdir_count(); i++) {
		if (_find_owners(efsd->get_subdir(i), p_path)) {
			return true;
		}
	}

	for (int i = 0; i < efsd->get_file_count(); i++) {
		Vector<String> deps = efsd->get_file_deps(i);
		if (deps.find(p_path) != -1)
			return true;
	}

	return false;
}
void ImportDock::_reimport_attempt() {
	bool need_restart = false;
	bool used_in_resources = false;

	String importer_name;
	if (params->importer.is_valid()) {
		importer_name = params->importer->get_importer_name();
	} else {
		importer_name = "keep";
	}
	for (int i = 0; i < params->paths.size(); i++) {
		Ref<ConfigFile> config = _get_config(params->paths[i]);
		ERR_CONTINUE(config.is_null());

		String imported_with = config->get_value("remap", "importer");
		if (imported_with != importer_name) {
			need_restart = true;
			if (_find_owners(EditorFileSystem::get_singleton()->get_filesystem(), params->paths[i])) {
				used_in_resources = true;
			}
		}
	}

	if (need_restart) {
		label_warning->set_visible(used_in_resources);
		reimport_confirm->popup_centered_minsize();
		return;
	}

	_reimport();
}

void ImportDock::_reimport_and_restart() {
	EditorNode::get_singleton()->save_all_scenes();
	EditorResourcePreview::get_singleton()->stop(); //don't try to re-create previews after import
	_reimport();
	EditorNode::get_singleton()->restart_editor();
}

void ImportDock::_reimport() {
	for (int i = 0; i < params->paths.size(); i++) {
		Ref<ConfigFile> config = _get_config(params->paths[i]);
		ERR_CONTINUE(config.is_null());

		if (params->importer.is_valid()) {
			String importer_name = params->importer->get_importer_name();

			if (params->checking && config->get_value("remap", "importer") == params->importer->get_importer_name()) {
				//update only what is edited (checkboxes) if the importer is the same
				for (List<PropertyInfo>::Element *E = params->properties.front(); E; E = E->next()) {
					if (params->checked.has(E->get().name)) {
						config->set_value("params", E->get().name, params->values[E->get().name]);
					}
				}
			} else {
				//override entirely
				config->set_value("remap", "importer", importer_name);
				if (config->has_section("params")) {
					config->erase_section("params");
				}

				for (List<PropertyInfo>::Element *E = params->properties.front(); E; E = E->next()) {
					config->set_value("params", E->get().name, params->values[E->get().name]);
				}
			}

			//handle group file
			Ref<ResourceImporter> importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(importer_name);
			ERR_CONTINUE(!importer.is_valid());
			String group_file_property = importer->get_option_group_file();
			if (group_file_property != String()) {
				//can import from a group (as in, atlas)
				ERR_CONTINUE(!params->values.has(group_file_property));
				String group_file = params->values[group_file_property];
				config->set_value("remap", "group_file", group_file);
			} else {
				config->set_value("remap", "group_file", Variant()); //clear group file if unused
			}
		} else {
			//set to no import
			config->clear();
			config->set_value("remap", "importer", "keep");
		}

		config->save(params->paths[i] + ".import");
	}

	EditorFileSystem::get_singleton()->reimport_files(params->paths);
	EditorFileSystem::get_singleton()->emit_signal("filesystem_changed"); //it changed, so force emitting the signal
}

void ImportDock::_notification(int p_what) {
	switch (p_what) {
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			imported->add_style_override("normal", get_stylebox("normal", "LineEdit"));
		} break;

		case NOTIFICATION_ENTER_TREE: {
			import_opts->edit(params);
			label_warning->add_color_override("font_color", get_color("warning_color", "Editor"));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible()) {
				if (files_to_import.size() == 1) {
					// ??? A crash can occur in set_edit_path when accessing p_path,
					// if files_to_import[0] is passed directly.
					String file = files_to_import[0];
					set_edit_path(file);
				} else if (files_to_import.size() > 1) {
					// Copy the vector because set_edit_multiple_paths resets files_to_import.
					Vector<String> files = files_to_import;
					set_edit_multiple_paths(files);
				}
			} else {
				clear();
				config_cache.clear();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// If the ImportDock or the window closes while processing,
			// stop the thread so the editor doesn't crash.
			clear();
		} break;
	}
}

void ImportDock::_property_toggled(const StringName &p_prop, bool p_checked) {
	if (p_checked) {
		params->checked.insert(p_prop);
	} else {
		params->checked.erase(p_prop);
	}
}
void ImportDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_reimport"), &ImportDock::_reimport);
	ClassDB::bind_method(D_METHOD("_preset_selected"), &ImportDock::_preset_selected);
	ClassDB::bind_method(D_METHOD("_importer_selected"), &ImportDock::_importer_selected);
	ClassDB::bind_method(D_METHOD("_property_toggled"), &ImportDock::_property_toggled);
	ClassDB::bind_method(D_METHOD("_reimport_and_restart"), &ImportDock::_reimport_and_restart);
	ClassDB::bind_method(D_METHOD("_reimport_attempt"), &ImportDock::_reimport_attempt);
	ClassDB::bind_method(D_METHOD("_finish_set_edit_multiple_paths"), &ImportDock::_finish_set_edit_multiple_paths);
}

void ImportDock::initialize_import_options() const {
	ERR_FAIL_COND(!import_opts || !params);

	import_opts->edit(params);
}

ImportDock::ImportDock() {
	set_name("Import");
	imported = memnew(Label);
	imported->add_style_override("normal", EditorNode::get_singleton()->get_gui_base()->get_stylebox("normal", "LineEdit"));
	imported->set_clip_text(true);
	add_child(imported);
	HBoxContainer *hb = memnew(HBoxContainer);
	add_margin_child(TTR("Import As:"), hb);
	import_as = memnew(OptionButton);
	import_as->set_disabled(true);
	import_as->connect("item_selected", this, "_importer_selected");
	hb->add_child(import_as);
	import_as->set_h_size_flags(SIZE_EXPAND_FILL);
	preset = memnew(MenuButton);
	preset->set_text(TTR("Preset"));
	preset->set_disabled(true);
	preset->get_popup()->connect("index_pressed", this, "_preset_selected");
	hb->add_child(preset);

	import_opts = memnew(EditorInspector);
	add_child(import_opts);
	import_opts->set_v_size_flags(SIZE_EXPAND_FILL);
	import_opts->connect("property_toggled", this, "_property_toggled");

	hb = memnew(HBoxContainer);
	add_child(hb);
	import = memnew(Button);
	import->set_text(TTR("Reimport"));
	import->set_disabled(true);
	import->connect("pressed", this, "_reimport_attempt");
	hb->add_spacer();
	hb->add_child(import);
	hb->add_spacer();

	reimport_confirm = memnew(ConfirmationDialog);
	reimport_confirm->get_ok()->set_text(TTR("Save Scenes, Re-Import, and Restart"));
	add_child(reimport_confirm);
	reimport_confirm->connect("confirmed", this, "_reimport_and_restart");

	VBoxContainer *vbc_confirm = memnew(VBoxContainer());
	vbc_confirm->add_child(memnew(Label(TTR("Changing the type of an imported file requires editor restart."))));
	label_warning = memnew(Label(TTR("WARNING: Assets exist that use this resource, they may stop loading properly.")));
	vbc_confirm->add_child(label_warning);
	reimport_confirm->add_child(vbc_confirm);

	params = memnew(ImportDockParameters);
}

ImportDock::~ImportDock() {
	memdelete(params);
}
