/*************************************************************************/
/*  gdscript_language_protocol.cpp                                       */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2020 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2020 Godot Engine contributors (cf. AUTHORS.md).   */
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

#include "gdscript_language_protocol.h"
#include "core/io/json.h"
#include "core/os/copymem.h"
#include "core/project_settings.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"

GDScriptLanguageProtocol *GDScriptLanguageProtocol::singleton = NULL;

void GDScriptLanguageProtocol::on_data_received(int bytes, Ref<StreamPeerTCP> peer) {
	// Check if header is big enough
	if (bytes > 20) {
		//Try to read "Content-Length:<num>\r\n\r\n" header
		uint8_t *header_start = (uint8_t *)memalloc(16);
		peer->get_data(header_start, 16);
		bytes -= 16;
		String header;
		header.parse_utf8((const char *)header_start, 16);
		memfree(header_start);
		ERR_FAIL_COND(!header.begins_with("Content-Length: "));

		// Read bytes until end of header
		while (!header.ends_with("\r\n\r\n")) {
			uint8_t *buff = (uint8_t *)memalloc(1);
			peer->get_data(buff, 1);
			bytes -= 1;
			String str_buff;
			str_buff.parse_utf8((const char *)buff, 1);
			header += str_buff;
			memfree(buff);
		}

		int content_length = header.substr(16, header.size()).to_int();
		uint8_t *content = (uint8_t *)memalloc(content_length);
		if (OK == peer->get_data(content, content_length)) {
			String message;
			message.parse_utf8((const char *)content, content_length);
			String output = process_message(message);
			if (!output.empty()) {
				CharString charstr = output.utf8();
				peer->put_data((const uint8_t *)charstr.ptr(), charstr.length());
			}
		}
		memfree(content);
	} else {
		EditorNode::get_log()->add_message("Unable to parse header", EditorLog::MSG_TYPE_EDITOR);
		uint8_t *data = (uint8_t *)memalloc(bytes);
		peer->get_data(data, bytes); // Flush buffer
		memfree(data);
	}
}

void GDScriptLanguageProtocol::on_client_connected(Ref<StreamPeerTCP> peer) {
  if (peer != NULL) {
    peers.push_back(peer);
    EditorNode::get_log()->add_message("Connection Taken", EditorLog::MSG_TYPE_EDITOR);

    // TODO: figure out proper way to get this params when each client connects
    Dictionary p_params;
    String root_uri = p_params["rootUri"];
    String root = p_params["rootPath"];
    bool is_same_workspace;
#ifndef WINDOWS_ENABLED
    is_same_workspace = root.to_lower() == workspace->root.to_lower();
#else
    is_same_workspace = root.replace("\\", "/").to_lower() == workspace->root.to_lower();
#endif

    if (root_uri.length() && is_same_workspace) {
      workspace->root_uri = root_uri;
    } else {

      workspace->root_uri = "file://" + workspace->root;

      Dictionary params;
      params["path"] = workspace->root;
      Dictionary request = make_notification("gdscript_client/changeWorkspace", params);

      String msg = JSON::print(request);
      msg = format_output(msg);
      CharString charstr = msg.utf8();
      peer->put_data((const uint8_t *)charstr.ptr(), charstr.length());
    }
  }
}

void GDScriptLanguageProtocol::on_client_disconnected() {
	EditorNode::get_log()->add_message("Disconnected", EditorLog::MSG_TYPE_EDITOR);
}

String GDScriptLanguageProtocol::process_message(const String &p_text) {
	String ret = process_string(p_text);
	if (ret.empty()) {
		return ret;
	} else {
		return format_output(ret);
	}
}

String GDScriptLanguageProtocol::format_output(const String &p_text) {

	String header = "Content-Length: ";
	CharString charstr = p_text.utf8();
	size_t len = charstr.length();
	header += itos(len);
	header += "\r\n\r\n";

	return header + p_text;
}

void GDScriptLanguageProtocol::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "params"), &GDScriptLanguageProtocol::initialize);
	ClassDB::bind_method(D_METHOD("initialized", "params"), &GDScriptLanguageProtocol::initialized);
	ClassDB::bind_method(D_METHOD("on_data_received"), &GDScriptLanguageProtocol::on_data_received);
	ClassDB::bind_method(D_METHOD("on_client_connected"), &GDScriptLanguageProtocol::on_client_connected);
	ClassDB::bind_method(D_METHOD("on_client_disconnected"), &GDScriptLanguageProtocol::on_client_disconnected);
	ClassDB::bind_method(D_METHOD("notify_client", "p_method", "p_params"), &GDScriptLanguageProtocol::notify_client, DEFVAL(Variant()), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("is_smart_resolve_enabled"), &GDScriptLanguageProtocol::is_smart_resolve_enabled);
	ClassDB::bind_method(D_METHOD("get_text_document"), &GDScriptLanguageProtocol::get_text_document);
	ClassDB::bind_method(D_METHOD("get_workspace"), &GDScriptLanguageProtocol::get_workspace);
	ClassDB::bind_method(D_METHOD("is_initialized"), &GDScriptLanguageProtocol::is_initialized);
}

Dictionary GDScriptLanguageProtocol::initialize(const Dictionary &p_params) {

	lsp::InitializeResult ret;

	if (!_initialized) {
		workspace->initialize();
		text_document->initialize();
		_initialized = true;
	}

	return ret.to_json();
}

void GDScriptLanguageProtocol::initialized(const Variant &p_params) {

	lsp::GodotCapabilities capabilities;

	DocData *doc = EditorHelp::get_doc_data();
	for (Map<String, DocData::ClassDoc>::Element *E = doc->class_list.front(); E; E = E->next()) {

		lsp::GodotNativeClassInfo gdclass;
		gdclass.name = E->get().name;
		gdclass.class_doc = &(E->get());
		if (ClassDB::ClassInfo *ptr = ClassDB::classes.getptr(StringName(E->get().name))) {
			gdclass.class_info = ptr;
		}
		capabilities.native_classes.push_back(gdclass);
	}

	notify_client("gdscript/capabilities", capabilities.to_json());
}

void GDScriptLanguageProtocol::poll() {
	if (server->is_connection_available()) {
    Ref<StreamPeerTCP> peer = server->take_connection();
		on_client_connected(peer);
	}
  std::vector<std::vector<Ref<StreamPeerTCP>>::iterator> toDelete;
  for (std::vector<Ref<StreamPeerTCP>>::iterator it = peers.begin(); it != peers.end(); ++it) {
		StreamPeerTCP::Status status = (*it)->get_status();
		if (status == StreamPeerTCP::STATUS_NONE || status == StreamPeerTCP::STATUS_ERROR) {
      toDelete.push_back(it);
			on_client_disconnected();
		} else {
			int bytes = (*it)->get_available_bytes();
			if (bytes > 0) {
				on_data_received(bytes, (*it));
			}
		}
  }
  for (std::vector<std::vector<Ref<StreamPeerTCP>>::iterator>::iterator it = toDelete.begin(); it != toDelete.end(); ++it) {
    peers.erase(*it);
  }
}

Error GDScriptLanguageProtocol::start(int p_port, const IP_Address &p_bind_ip) {
	if (server == NULL) {
		server = dynamic_cast<TCP_Server *>(ClassDB::instance("TCP_Server"));
		ERR_FAIL_COND_V(!server, FAILED);
	}
	return server->listen(p_port, p_bind_ip);
}

void GDScriptLanguageProtocol::stop() {
  if (!peers.empty()) {
    for (std::vector<Ref<StreamPeerTCP>>::iterator it = peers.begin(); it != peers.end(); ++it) {
      (*it)->disconnect_from_host();
    }
  }
  server->stop();
}

void GDScriptLanguageProtocol::notify_client(const String &p_method, const Variant &p_params) {
	ERR_FAIL_COND(peers.empty());

	Dictionary message = make_notification(p_method, p_params);
	String msg = JSON::print(message);
	msg = format_output(msg);

  for (std::vector<Ref<StreamPeerTCP>>::iterator it = peers.begin(); it != peers.end(); ++it) {
    (*it)->put_string(msg);
  }
}

bool GDScriptLanguageProtocol::is_smart_resolve_enabled() const {
	return bool(_EDITOR_GET("network/language_server/enable_smart_resolve"));
}

bool GDScriptLanguageProtocol::is_goto_native_symbols_enabled() const {
	return bool(_EDITOR_GET("network/language_server/show_native_symbols_in_editor"));
}

GDScriptLanguageProtocol::GDScriptLanguageProtocol() {
	server = NULL;
	singleton = this;
	_initialized = false;
	workspace.instance();
	text_document.instance();
	set_scope("textDocument", text_document.ptr());
	set_scope("completionItem", text_document.ptr());
	set_scope("workspace", workspace.ptr());
	workspace->root = ProjectSettings::get_singleton()->get_resource_path();
}

GDScriptLanguageProtocol::~GDScriptLanguageProtocol() {
	memdelete(server);
	server = NULL;
}
