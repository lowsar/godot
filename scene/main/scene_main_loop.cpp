/*************************************************************************/
/*  scene_main_loop.cpp                                                  */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                    http://www.godotengine.org                         */
/*************************************************************************/
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
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
#include "scene_main_loop.h"

#include "print_string.h"
#include "os/os.h"
#include "message_queue.h"
#include "node.h"
#include "globals.h"
#include <stdio.h>
#include "os/keyboard.h"
#include "servers/spatial_sound_2d_server.h"
#include "servers/physics_2d_server.h"
#include "servers/physics_server.h"
#include "scene/scene_string_names.h"
#include "io/resource_loader.h"
#include "viewport.h"


void SceneMainLoop::tree_changed() {

	tree_version++;
	emit_signal(tree_changed_name);
}

void SceneMainLoop::node_removed(Node *p_node) {

	emit_signal(node_removed_name,p_node);
	if (call_lock>0)
		call_skip.insert(p_node);


}


void SceneMainLoop::add_to_group(const StringName& p_group, Node *p_node) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E) {
		E=group_map.insert(p_group,Group());
	}

	if (E->get().nodes.find(p_node)!=-1) {
		ERR_EXPLAIN("Already in group: "+p_group);
		ERR_FAIL();
	}
	E->get().nodes.push_back(p_node);
	E->get().last_tree_version=0;
}

void SceneMainLoop::remove_from_group(const StringName& p_group, Node *p_node) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	ERR_FAIL_COND(!E);


	E->get().nodes.erase(p_node);
	if (E->get().nodes.empty())
		group_map.erase(E);
}

void SceneMainLoop::_flush_transform_notifications() {

	SelfList<Node>* n = xform_change_list.first();
	while(n) {

		Node *node=n->self();
		SelfList<Node>* nx = n->next();
		xform_change_list.remove(n);
		n=nx;
		node->notification(NOTIFICATION_TRANSFORM_CHANGED);
	}
}

void SceneMainLoop::_flush_ugc() {

	ugc_locked=true;

	while (unique_group_calls.size()) {

		Map<UGCall,Vector<Variant> >::Element *E=unique_group_calls.front();

		Variant v[VARIANT_ARG_MAX];
		for(int i=0;i<E->get().size();i++)
			v[i]=E->get()[i];

		call_group(GROUP_CALL_REALTIME,E->key().group,E->key().call,v[0],v[1],v[2],v[3],v[4]);

		unique_group_calls.erase(E);
	}

	ugc_locked=false;
}

void SceneMainLoop::_update_group_order(Group& g) {

	if (g.last_tree_version==tree_version)
		return;
	if (g.nodes.empty())
		return;

	Node **nodes = &g.nodes[0];
	int node_count=g.nodes.size();

	SortArray<Node*,Node::Comparator> node_sort;
	node_sort.sort(nodes,node_count);
	g.last_tree_version=tree_version;
}


void SceneMainLoop::call_group(uint32_t p_call_flags,const StringName& p_group,const StringName& p_function,VARIANT_ARG_DECLARE) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;
	Group &g=E->get();
	if (g.nodes.empty())
		return;

	_update_group_order(g);


	if (p_call_flags&GROUP_CALL_UNIQUE && !(p_call_flags&GROUP_CALL_REALTIME)) {

		ERR_FAIL_COND(ugc_locked);

		UGCall ug;
		ug.call=p_function;
		ug.group=p_group;

		if (unique_group_calls.has(ug))
			return;

		VARIANT_ARGPTRS;

		Vector<Variant> args;
		for(int i=0;i<VARIANT_ARG_MAX;i++) {
			if (argptr[i]->get_type()==Variant::NIL)
				break;
			args.push_back(*argptr[i]);
		}

		unique_group_calls[ug]=args;
		return;
	}

	Vector<Node*> nodes_copy = g.nodes;
	Node **nodes = &nodes_copy[0];
	int node_count=nodes_copy.size();

	call_lock++;

	if (p_call_flags&GROUP_CALL_REVERSE) {

		for(int i=node_count-1;i>=0;i--) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME) {
				if (p_call_flags&GROUP_CALL_MULIILEVEL)
					nodes[i]->call_multilevel(p_function,VARIANT_ARG_PASS);
				else
					nodes[i]->call(p_function,VARIANT_ARG_PASS);
			} else
				MessageQueue::get_singleton()->push_call(nodes[i],p_function,VARIANT_ARG_PASS);

		}

	} else {

		for(int i=0;i<node_count;i++) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME) {
				if (p_call_flags&GROUP_CALL_MULIILEVEL)
					nodes[i]->call_multilevel(p_function,VARIANT_ARG_PASS);
				else
					nodes[i]->call(p_function,VARIANT_ARG_PASS);
			} else
				MessageQueue::get_singleton()->push_call(nodes[i],p_function,VARIANT_ARG_PASS);			
		}

	}

	call_lock--;
	if (call_lock==0)
		call_skip.clear();
}

void SceneMainLoop::notify_group(uint32_t p_call_flags,const StringName& p_group,int p_notification) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;
	Group &g=E->get();
	if (g.nodes.empty())
		return;

	_update_group_order(g);

	Vector<Node*> nodes_copy = g.nodes;
	Node **nodes = &nodes_copy[0];
	int node_count=nodes_copy.size();

	call_lock++;

	if (p_call_flags&GROUP_CALL_REVERSE) {

		for(int i=node_count-1;i>=0;i--) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME)
				nodes[i]->notification(p_notification);
			else
				MessageQueue::get_singleton()->push_notification(nodes[i],p_notification);
		}

	} else {

		for(int i=0;i<node_count;i++) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME)
				nodes[i]->notification(p_notification);
			else
				MessageQueue::get_singleton()->push_notification(nodes[i],p_notification);
		}

	}

	call_lock--;
	if (call_lock==0)
		call_skip.clear();
}

void SceneMainLoop::set_group(uint32_t p_call_flags,const StringName& p_group,const String& p_name,const Variant& p_value) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;
	Group &g=E->get();
	if (g.nodes.empty())
		return;

	_update_group_order(g);

	Vector<Node*> nodes_copy = g.nodes;
	Node **nodes = &nodes_copy[0];
	int node_count=nodes_copy.size();

	call_lock++;

	if (p_call_flags&GROUP_CALL_REVERSE) {

		for(int i=node_count-1;i>=0;i--) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME)
				nodes[i]->set(p_name,p_value);
			else
				MessageQueue::get_singleton()->push_set(nodes[i],p_name,p_value);
		}

	} else {

		for(int i=0;i<node_count;i++) {

			if (call_lock && call_skip.has(nodes[i]))
				continue;

			if (p_call_flags&GROUP_CALL_REALTIME)
				nodes[i]->set(p_name,p_value);
			else
				MessageQueue::get_singleton()->push_set(nodes[i],p_name,p_value);
		}

	}

	call_lock--;
	if (call_lock==0)
		call_skip.clear();
}

void SceneMainLoop::set_input_as_handled() {

	input_handled=true;
}

void SceneMainLoop::input_text( const String& p_text ) {

	root_lock++;

	call_group(GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"input",p_text);
	root_lock--;

}

void SceneMainLoop::input_event( const InputEvent& p_event ) {


	root_lock++;
	last_id=p_event.ID;

	input_handled=false;


	InputEvent ev = p_event;
#if 0
	switch(ev.type) {

		case InputEvent::MOUSE_BUTTON: {

			Matrix32 ai = root->get_final_transform().affine_inverse();
			Vector2 g = ai.xform(Vector2(ev.mouse_button.global_x,ev.mouse_button.global_y));
			Vector2 l = ai.xform(Vector2(ev.mouse_button.x,ev.mouse_button.y));
			ev.mouse_button.x=l.x;
			ev.mouse_button.y=l.y;
			ev.mouse_button.global_x=g.x;
			ev.mouse_button.global_y=g.y;

		} break;
		case InputEvent::MOUSE_MOTION: {

			Matrix32 ai = root->get_final_transform().affine_inverse();
			Vector2 g = ai.xform(Vector2(ev.mouse_motion.global_x,ev.mouse_motion.global_y));
			Vector2 l = ai.xform(Vector2(ev.mouse_motion.x,ev.mouse_motion.y));
			Vector2 r = ai.xform(Vector2(ev.mouse_motion.relative_x,ev.mouse_motion.relative_y));
			ev.mouse_motion.x=l.x;
			ev.mouse_motion.y=l.y;
			ev.mouse_motion.global_x=g.x;
			ev.mouse_motion.global_y=g.y;
			ev.mouse_motion.relative_x=r.x;
			ev.mouse_motion.relative_y=r.y;

		} break;
		case InputEvent::SCREEN_TOUCH: {

			Matrix32 ai = root->get_final_transform().affine_inverse();
			Vector2 t = ai.xform(Vector2(ev.screen_touch.x,ev.screen_touch.y));
			ev.screen_touch.x=t.x;
			ev.screen_touch.y=t.y;

		} break;
		case InputEvent::SCREEN_DRAG: {

			Matrix32 ai = root->get_final_transform().affine_inverse();
			Vector2 t = ai.xform(Vector2(ev.screen_drag.x,ev.screen_drag.y));
			Vector2 r = ai.xform(Vector2(ev.screen_drag.relative_x,ev.screen_drag.relative_y));
			Vector2 s = ai.xform(Vector2(ev.screen_drag.speed_x,ev.screen_drag.speed_y));
			ev.screen_drag.x=t.x;
			ev.screen_drag.y=t.y;
			ev.screen_drag.relative_x=r.x;
			ev.screen_drag.relative_y=r.y;
			ev.screen_drag.speed_x=s.x;
			ev.screen_drag.speed_y=s.y;
		} break;
	}

#endif

	MainLoop::input_event(p_event);
#if 0
	_call_input_pause("input","_input",ev);

	call_group(GROUP_CALL_REVERSE|GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"_gui_input","_gui_input",p_event); //special one for GUI, as controls use their own process check

	//call_group(GROUP_CALL_REVERSE|GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"input","_input",ev);

	/*if (ev.type==InputEvent::KEY && ev.key.pressed && !ev.key.echo && ev.key.scancode==KEY_F12) {

		print_line("RAM: "+itos(Memory::get_static_mem_usage()));
		print_line("DRAM: "+itos(Memory::get_dynamic_mem_usage()));
	}
*/
	//if (ev.type==InputEvent::KEY && ev.key.pressed && !ev.key.echo && ev.key.scancode==KEY_F11) {

	//	Memory::dump_static_mem_to_file("memdump.txt");
	//}

	//transform for the rest
#else

	call_group(GROUP_CALL_REALTIME,"_viewports","_vp_input",p_event); //special one for GUI, as controls use their own process check

#endif
	if (ScriptDebugger::get_singleton() && ScriptDebugger::get_singleton()->is_remote() && ev.type==InputEvent::KEY && ev.key.pressed && !ev.key.echo && ev.key.scancode==KEY_F8) {

		ScriptDebugger::get_singleton()->request_quit();
	}

	_flush_ugc();	
	root_lock--;
	MessageQueue::get_singleton()->flush(); //small little hack

	root_lock++;

	if (!input_handled) {

#if 0
		_call_input_pause("unhandled_input","_unhandled_input",ev);
		//call_group(GROUP_CALL_REVERSE|GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"unhandled_input","_unhandled_input",ev);
		if (!input_handled && ev.type==InputEvent::KEY) {
			_call_input_pause("unhandled_key_input","_unhandled_key_input",ev);
			//call_group(GROUP_CALL_REVERSE|GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"unhandled_key_input","_unhandled_key_input",ev);
		}
#else

		call_group(GROUP_CALL_REALTIME,"_viewports","_vp_unhandled_input",p_event); //special one for GUI, as controls use their own process check

#endif
		input_handled=true;
		_flush_ugc();
		root_lock--;
		MessageQueue::get_singleton()->flush(); //small little hack
	} else {
		input_handled=true;
		root_lock--;

	}

}

void SceneMainLoop::init() {

	//_quit=false;
	accept_quit=true;
	initialized=true;
	input_handled=false;


	editor_hint=false;
	pause=false;

	root->_set_scene(this);
	MainLoop::init();

}

bool SceneMainLoop::iteration(float p_time) {


	root_lock++;

	current_frame++;

	_flush_transform_notifications();

	MainLoop::iteration(p_time);

	fixed_process_time=p_time;
	_notify_group_pause("fixed_process",Node::NOTIFICATION_FIXED_PROCESS);
	_flush_ugc();
	_flush_transform_notifications();
	call_group(GROUP_CALL_REALTIME,"_viewports","update_worlds");
	root_lock--;

	_flush_delete_queue();

	return _quit;
}

bool SceneMainLoop::idle(float p_time){


//	print_line("ram: "+itos(OS::get_singleton()->get_static_memory_usage())+" sram: "+itos(OS::get_singleton()->get_dynamic_memory_usage()));
//	print_line("node count: "+itos(get_node_count()));
//	print_line("TEXTURE RAM: "+itos(VS::get_singleton()->get_render_info(VS::INFO_TEXTURE_MEM_USED)));

	root_lock++;

	MainLoop::idle(p_time);

	idle_process_time=p_time;

	_flush_transform_notifications();

	_notify_group_pause("idle_process",Node::NOTIFICATION_PROCESS);

	Size2 win_size=Size2( OS::get_singleton()->get_video_mode().width, OS::get_singleton()->get_video_mode().height );
	if(win_size!=last_screen_size) {


		last_screen_size=win_size;
		root->set_rect(Rect2(Point2(),last_screen_size));

		emit_signal("screen_resized");

	}

	_flush_ugc();
	_flush_transform_notifications(); //transforms after world update, to avoid unnecesary enter/exit notifications
	call_group(GROUP_CALL_REALTIME,"_viewports","update_worlds");

	root_lock--;

	_flush_delete_queue();

	return _quit;
}

void SceneMainLoop::finish() {

	_flush_delete_queue();

	_flush_ugc();

	initialized=false;

	MainLoop::finish();

	if (root) {
		root->_set_scene(NULL);
		memdelete(root); //delete root
	}









}


void SceneMainLoop::quit() {

	_quit=true;
}

void SceneMainLoop::_notification(int p_notification) {



	switch (p_notification) {

		case NOTIFICATION_WM_QUIT_REQUEST: {

			get_root()->propagate_notification(p_notification);

			if (accept_quit) {
				_quit=true;
				break;
			}
		} break;
		case NOTIFICATION_WM_FOCUS_IN:
		case NOTIFICATION_WM_FOCUS_OUT: {

			get_root()->propagate_notification(p_notification);
		} break;
		case NOTIFICATION_WM_UNFOCUS_REQUEST: {

			notify_group(GROUP_CALL_REALTIME|GROUP_CALL_MULIILEVEL,"input",NOTIFICATION_WM_UNFOCUS_REQUEST);

		} break;

		default:
			break;
	};
};


void SceneMainLoop::set_auto_accept_quit(bool p_enable) {

	accept_quit=p_enable;
}

void SceneMainLoop::set_editor_hint(bool p_enabled) {

	editor_hint=p_enabled;
}

bool SceneMainLoop::is_editor_hint() const {

	return editor_hint;
}

void SceneMainLoop::set_pause(bool p_enabled) {

	if (p_enabled==pause)
		return;
	pause=p_enabled;
	PhysicsServer::get_singleton()->set_active(!p_enabled);
	Physics2DServer::get_singleton()->set_active(!p_enabled);
	if (get_root())
		get_root()->propagate_notification(p_enabled ? Node::NOTIFICATION_PAUSED : Node::NOTIFICATION_UNPAUSED);
}

bool SceneMainLoop::is_paused() const {

	return pause;
}

void SceneMainLoop::_call_input_pause(const StringName& p_group,const StringName& p_method,const InputEvent& p_input) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;
	Group &g=E->get();
	if (g.nodes.empty())
		return;

	_update_group_order(g);

	//copy, so copy on write happens in case something is removed from process while being called
	//performance is not lost because only if something is added/removed the vector is copied.
	Vector<Node*> nodes_copy = g.nodes;

	int node_count=nodes_copy.size();
	Node **nodes = &nodes_copy[0];

	Variant arg=p_input;
	const Variant *v[1]={&arg};

	call_lock++;

	for(int i=node_count-1;i>=0;i--) {

		if (input_handled)
			break;

		Node *n = nodes[i];
		if (call_lock && call_skip.has(n))
			continue;

		if (!n->can_process())
			continue;

		Variant::CallError ce;
		n->call_multilevel(p_method,(const Variant**)v,1);
		//ERR_FAIL_COND(node_count != g.nodes.size());
	}

	call_lock--;
	if (call_lock==0)
		call_skip.clear();
}

void SceneMainLoop::_notify_group_pause(const StringName& p_group,int p_notification) {

	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;
	Group &g=E->get();
	if (g.nodes.empty())
		return;


	_update_group_order(g);

	//copy, so copy on write happens in case something is removed from process while being called
	//performance is not lost because only if something is added/removed the vector is copied.
	Vector<Node*> nodes_copy = g.nodes;

	int node_count=nodes_copy.size();
	Node **nodes = &nodes_copy[0];

	call_lock++;

	for(int i=0;i<node_count;i++) {

		Node *n = nodes[i];
		if (call_lock && call_skip.has(n))
			continue;

		if (!n->can_process())
			continue;

		n->notification(p_notification);
		//ERR_FAIL_COND(node_count != g.nodes.size());
	}

	call_lock--;
	if (call_lock==0)
		call_skip.clear();
}

/*
void SceneMainLoop::_update_listener_2d() {

	if (listener_2d.is_valid()) {

		SpatialSound2DServer::get_singleton()->listener_set_space( listener_2d, world_2d->get_sound_space() );
	}

}
*/

uint32_t SceneMainLoop::get_last_event_id() const {

	return last_id;
}


Variant SceneMainLoop::_call_group(const Variant** p_args, int p_argcount, Variant::CallError& r_error) {


	r_error.error=Variant::CallError::CALL_OK;

	ERR_FAIL_COND_V(p_argcount<3,Variant());
	ERR_FAIL_COND_V(!p_args[0]->is_num(),Variant());
	ERR_FAIL_COND_V(p_args[1]->get_type()!=Variant::STRING,Variant());
	ERR_FAIL_COND_V(p_args[2]->get_type()!=Variant::STRING,Variant());

	int flags = *p_args[0];
	StringName group = *p_args[1];
	StringName method = *p_args[2];
	Variant v[VARIANT_ARG_MAX];

	for(int i=0;i<MIN(p_argcount-3,5);i++) {

		v[i]=*p_args[i+3];
	}

	call_group(flags,group,method,v[0],v[1],v[2],v[3],v[4]);
	return Variant();
}


int64_t SceneMainLoop::get_frame() const {

	return current_frame;
}


Array SceneMainLoop::_get_nodes_in_group(const StringName& p_group) {

	Array ret;
	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return ret;

	_update_group_order(E->get()); //update order just in case
	int nc = E->get().nodes.size();
	if (nc==0)
		return ret;

	ret.resize(nc);

	Node **ptr = E->get().nodes.ptr();
	for(int i=0;i<nc;i++) {

		ret[i]=ptr[i];
	}

	return ret;
}

void SceneMainLoop::get_nodes_in_group(const StringName& p_group,List<Node*> *p_list) {


	Map<StringName,Group>::Element *E=group_map.find(p_group);
	if (!E)
		return;

	_update_group_order(E->get()); //update order just in case
	int nc = E->get().nodes.size();
	if (nc==0)
		return;
	Node **ptr = E->get().nodes.ptr();
	for(int i=0;i<nc;i++) {

		p_list->push_back(ptr[i]);
	}
}


static void _fill_array(Node *p_node, Array& array, int p_level) {

	array.push_back(p_level);
	array.push_back(p_node->get_name());
	array.push_back(p_node->get_type());
	for(int i=0;i<p_node->get_child_count();i++) {

		_fill_array(p_node->get_child(i),array,p_level+1);
	}
}

void SceneMainLoop::_debugger_request_tree(void *self) {

	SceneMainLoop *sml = (SceneMainLoop *)self;

	Array arr;
	_fill_array(sml->root,arr,0);
	ScriptDebugger::get_singleton()->send_message("scene_tree",arr);
}


void SceneMainLoop::_flush_delete_queue() {

	_THREAD_SAFE_METHOD_

	while( delete_queue.size() ) {

		Object *obj = ObjectDB::get_instance( delete_queue.front()->get() );
		if (obj) {
			memdelete( obj );
		}
		delete_queue.pop_front();
	}
}

void SceneMainLoop::queue_delete(Object *p_object) {

	_THREAD_SAFE_METHOD_
	ERR_FAIL_NULL(p_object);
	delete_queue.push_back(p_object->get_instance_ID());
}


int SceneMainLoop::get_node_count() const {

	return node_count;
}

void SceneMainLoop::_bind_methods() {


	//ObjectTypeDB::bind_method(_MD("call_group","call_flags","group","method","arg1","arg2"),&SceneMainLoop::_call_group,DEFVAL(Variant()),DEFVAL(Variant()));
	ObjectTypeDB::bind_method(_MD("notify_group","call_flags","group","notification"),&SceneMainLoop::notify_group);
	ObjectTypeDB::bind_method(_MD("set_group","call_flags","group","property","value"),&SceneMainLoop::set_group);

	ObjectTypeDB::bind_method(_MD("get_nodes_in_group"),&SceneMainLoop::_get_nodes_in_group);

	ObjectTypeDB::bind_method(_MD("get_root:Viewport"),&SceneMainLoop::get_root);

	ObjectTypeDB::bind_method(_MD("set_auto_accept_quit","enabled"),&SceneMainLoop::set_auto_accept_quit);

	ObjectTypeDB::bind_method(_MD("set_editor_hint","enable"),&SceneMainLoop::set_editor_hint);
	ObjectTypeDB::bind_method(_MD("is_editor_hint"),&SceneMainLoop::is_editor_hint);

	ObjectTypeDB::bind_method(_MD("set_pause","enable"),&SceneMainLoop::set_pause);
	ObjectTypeDB::bind_method(_MD("is_paused"),&SceneMainLoop::is_paused);
	ObjectTypeDB::bind_method(_MD("set_input_as_handled"),&SceneMainLoop::set_input_as_handled);


	ObjectTypeDB::bind_method(_MD("get_node_count"),&SceneMainLoop::get_node_count);
	ObjectTypeDB::bind_method(_MD("get_frame"),&SceneMainLoop::get_frame);
	ObjectTypeDB::bind_method(_MD("quit"),&SceneMainLoop::quit);

	ObjectTypeDB::bind_method(_MD("queue_delete","obj"),&SceneMainLoop::queue_delete);


	MethodInfo mi;
	mi.name="call_group";
	mi.arguments.push_back( PropertyInfo( Variant::INT, "flags"));
	mi.arguments.push_back( PropertyInfo( Variant::STRING, "group"));
	mi.arguments.push_back( PropertyInfo( Variant::STRING, "method"));
	Vector<Variant> defargs;
	for(int i=0;i<VARIANT_ARG_MAX;i++) {
		mi.arguments.push_back( PropertyInfo( Variant::NIL, "arg"+itos(i)));
		defargs.push_back(Variant());
	}

	ObjectTypeDB::bind_native_method(METHOD_FLAGS_DEFAULT,"call_group",&SceneMainLoop::_call_group,mi,defargs);

	ADD_SIGNAL( MethodInfo("tree_changed") );
	ADD_SIGNAL( MethodInfo("node_removed",PropertyInfo( Variant::OBJECT, "node") ) );
	ADD_SIGNAL( MethodInfo("screen_resized") );

	BIND_CONSTANT( GROUP_CALL_DEFAULT );
	BIND_CONSTANT( GROUP_CALL_REVERSE );
	BIND_CONSTANT( GROUP_CALL_REALTIME );
	BIND_CONSTANT( GROUP_CALL_UNIQUE );

}

SceneMainLoop::SceneMainLoop() {

	_quit=false;
	initialized=false;
	tree_version=1;
	fixed_process_time=1;
	idle_process_time=1;
	last_id=0;
	root=NULL;
	current_frame=0;
	tree_changed_name="tree_changed";
	node_removed_name="node_removed";
	ugc_locked=false;
	call_lock=0;
	root_lock=0;
	node_count=0;

	//create with mainloop

	root = memnew( Viewport );
	root->set_name("root");
	root->set_world( Ref<World>( memnew( World )));
	//root->set_world_2d( Ref<World2D>( memnew( World2D )));
	root->set_as_audio_listener(true);
	root->set_as_audio_listener_2d(true);

	last_screen_size=Size2( OS::get_singleton()->get_video_mode().width, OS::get_singleton()->get_video_mode().height );
	root->set_rect(Rect2(Point2(),last_screen_size));

	if (ScriptDebugger::get_singleton()) {
		ScriptDebugger::get_singleton()->set_request_scene_tree_message_func(_debugger_request_tree,this);
	}


}


SceneMainLoop::~SceneMainLoop() {


}
