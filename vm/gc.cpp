/* The GC superclass methods, used by both GCs. */
#include "object_utils.hpp"
#include "gc.hpp"
#include "objectmemory.hpp"

#include "builtin/class.hpp"
#include "builtin/tuple.hpp"
#include "builtin/module.hpp"
#include "builtin/symbol.hpp"
#include "builtin/compiledmethod.hpp"
#include "call_frame.hpp"
#include "builtin/variable_scope.hpp"

namespace rubinius {

  ObjectMark::ObjectMark(GarbageCollector* gc) : gc(gc) { }

  Object* ObjectMark::call(Object* obj) {
    if(obj->reference_p()) {
      if(obj->zone == UnspecifiedZone) {
        std::cout << "USZ!\n";
        char* bad = (char*)0;
        if(*bad) exit(11);
      }
      sassert(obj->zone != UnspecifiedZone);
      return gc->saw_object(obj);
    }

    return NULL;
  }

  void ObjectMark::set(Object* target, Object** pos, Object* val) {
    *pos = val;
    if(val->reference_p()) {
      gc->object_memory->write_barrier(target, val);
    }
  }

  void ObjectMark::just_set(Object* target, Object* val) {
    if(val->reference_p()) {
      gc->object_memory->write_barrier(target, val);
    }
  }

  GarbageCollector::GarbageCollector(ObjectMemory *om)
                   :object_memory(om), weak_refs(NULL) { }

  /* Understands how to read the inside of an object and find all references
   * located within. It copies the objects pointed to, but does not follow into
   * those further (ie, not recursive) */
  void GarbageCollector::scan_object(Object* obj) {
    Object* slot;

    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during scan_object.\n";
    }

    if(obj->klass() && obj->klass()->reference_p()) {
      slot = saw_object(obj->klass());
      if(slot) object_memory->set_class(obj, slot);
    }

    if(obj->ivars() && obj->ivars()->reference_p()) {
      slot = saw_object(obj->ivars());
      if(slot) obj->ivars(object_memory->state, slot);
    }

    // If this object's refs are weak, then add it to the weak_refs
    // vector and don't look at it otherwise.
    if(obj->RefsAreWeak) {
      if(!weak_refs) {
        weak_refs = new ObjectArray(0);
      }

      weak_refs->push_back(obj);
      return;
    }

    TypeInfo* ti = object_memory->type_info[obj->obj_type];
    assert(ti);

    ObjectMark mark(this);
    ti->mark(obj, mark);
  }

  void GarbageCollector::delete_object(Object* obj) {
    if (obj->RequiresCleanup) {
      object_memory->find_type_info(obj)->cleanup(obj);
    }

    if(obj->Remember) {
      object_memory->unremember_object(obj);
    }
  }

  void GarbageCollector::saw_variable_scope(VariableScope* scope) {
    scope->update(mark_object(scope->self()),
                  mark_object(scope->module()),
                  mark_object(scope->block()));

    for(int i = 0; i < scope->number_of_locals(); i++) {
      Object* local = scope->get_local(i);
      if(local->reference_p()) {
        scope->set_local(i, mark_object(local));
      }
    }

    VariableScope* parent = scope->parent();
    if(parent && parent->reference_p()) {
      if(parent->stack_allocated_p()) {
        saw_variable_scope(parent);
      } else {
        scope->update_parent((VariableScope*)mark_object(parent));
      }
    }
  }

  void GarbageCollector::walk_call_frame(CallFrame* top_call_frame) {
    CallFrame* call_frame = top_call_frame;
    while(call_frame) {
      if(call_frame->name && call_frame->name->reference_p()) {
        call_frame->name = (Symbol*)mark_object(call_frame->name);
      }

      if(call_frame->cm && call_frame->cm->reference_p()) {
        call_frame->cm = (CompiledMethod*)mark_object(call_frame->cm);
      }

      for(int i = 0; i < call_frame->stack_size; i++) {
        Object* obj = call_frame->stk[i];
        if(obj && obj->reference_p()) {
          call_frame->stk[i] = mark_object(obj);
        }
      }

      if(call_frame->top_scope) {
        if(call_frame->top_scope->stack_allocated_p()) {
          saw_variable_scope(call_frame->top_scope);
        } else {
          call_frame->top_scope = (VariableScope*)mark_object(call_frame->top_scope);
        }
      }

      if(call_frame->scope) {
        if(call_frame->scope->stack_allocated_p()) {
          saw_variable_scope(call_frame->scope);
        } else {
          call_frame->scope = (VariableScope*)mark_object(call_frame->scope);
        }
      }

      call_frame = call_frame->previous;
    }
  }

  void GarbageCollector::visit_variable_scope(VariableScope* scope, ObjectVisitor& visit) {
    scope->update(visit.call(scope->self()),
                  visit.call(scope->module()),
                  visit.call(scope->block()));

    for(int i = 0; i < scope->number_of_locals(); i++) {
      Object* local = scope->get_local(i);
      if(local->reference_p()) {
        scope->set_local(i, visit.call(local));
      }
    }

    VariableScope* parent = scope->parent();
    if(parent && parent->reference_p()) {
      if(parent->stack_allocated_p()) {
        saw_variable_scope(parent);
      } else {
        scope->update_parent((VariableScope*)visit.call(parent));
      }
    }
  }

  void GarbageCollector::visit_call_frame(CallFrame* top_call_frame, ObjectVisitor& visit) {
    CallFrame* call_frame = top_call_frame;
    while(call_frame) {
      if(call_frame->name && call_frame->name->reference_p()) {
        call_frame->name = (Symbol*)visit.call(call_frame->name);
      }

      if(call_frame->cm && call_frame->cm->reference_p()) {
        call_frame->cm = (CompiledMethod*)visit.call(call_frame->cm);
      }

      for(int i = 0; i < call_frame->stack_size; i++) {
        Object* obj = call_frame->stk[i];
        if(obj && obj->reference_p()) {
          call_frame->stk[i] = visit.call(obj);
        }
      }

      if(call_frame->top_scope) {
        if(call_frame->top_scope->stack_allocated_p()) {
          visit_variable_scope(call_frame->top_scope, visit);
        } else {
          call_frame->top_scope = (VariableScope*)visit.call(call_frame->top_scope);
        }
      }

      if(call_frame->scope) {
        if(call_frame->scope->stack_allocated_p()) {
          visit_variable_scope(call_frame->scope, visit);
        } else {
          call_frame->scope = (VariableScope*)visit.call(call_frame->scope);
        }
      }

      call_frame = call_frame->previous;
    }
  }

  void GarbageCollector::visit_roots(Roots& roots, ObjectVisitor& visit) {
    Root* root = static_cast<Root*>(roots.head());
    while(root) {
      Object* tmp = root->get();
      if(tmp->reference_p()) {
        visit.call(tmp);
      }

      root = static_cast<Root*>(root->next());
    }
  }

  void GarbageCollector::visit_call_frames_list(CallFrameLocationList& call_frames,
      ObjectVisitor& visit) {

    // Walk all the call frames
    for(CallFrameLocationList::const_iterator i = call_frames.begin();
        i != call_frames.end();
        i++) {
      CallFrame** loc = *i;
      visit_call_frame(*loc, visit);
    }
  }

  class UnmarkVisitor : public ObjectVisitor {
    std::vector<Object*> stack_;
    ObjectMemory* object_memory_;

  public:

    UnmarkVisitor(ObjectMemory* om)
      : object_memory_(om)
    {}

    Object* call(Object* obj) {
      if(watched_p(obj)) {
        std::cout << "detected " << obj << " during unmarking.\n";
      }

      if(obj->reference_p() && obj->marked_p()) {
        obj->clear_mark();
        stack_.push_back(obj);
      }

      return obj;
    }

    /* Understands how to read the inside of an object and find all references
     * located within. It copies the objects pointed to, but does not follow into
     * those further (ie, not recursive) */
    void visit_object(Object* obj) {
      if(obj->klass() && obj->klass()->reference_p()) {
        call(obj->klass());
      }

      if(obj->ivars() && obj->ivars()->reference_p()) {
        call(obj->ivars());
      }

      TypeInfo* ti = object_memory_->type_info[obj->obj_type];
      assert(ti);

      ti->visit(obj, *this);
    }

    void drain_stack() {
      while(!stack_.empty()) {
        Object* obj = stack_.back();
        stack_.pop_back();

        if(watched_p(obj)) {
          std::cout << "detected " << obj << " in unmarking stack.\n";
        }

        visit_object(obj);
      }
    }
  };

  void GarbageCollector::unmark_all(Roots &roots, CallFrameLocationList& call_frames) {
    UnmarkVisitor visit(object_memory);

    visit_roots(roots, visit);
    visit_call_frames_list(call_frames, visit);

    visit.drain_stack();
  }
}
