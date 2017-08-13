#pragma once


#include <Atomic/Container/Vector.h>
#include <Atomic/Core/Object.h>
#include <Atomic/Scene/Serializable.h>
#include <Atomic/IO/Log.h>

#include <UrhoUI.h>

using namespace Atomic;
using namespace Atomic::UrhoUI;

struct UndoState
{
    enum Type
    {
        ATTRIBUTE_CHANGED,
        UI_ADD,
        UI_REMOVE,
    } type;

    /// Object that was modified.
    SharedPtr<Serializable> item;
    /// Name of attribute.
    String attribute_name;
    /// Stored attribute value.
    Variant attribute_value;

    /// Parent of `element`.
    SharedPtr<Serializable> parent;
    /// Index of `element` in children list of `parent`.
    unsigned index;
};


class UndoManager : public Object
{
    ATOMIC_OBJECT(UndoManager, Object);
public:
    UndoManager(Context* ctx) : Object(ctx) { }

    void Undo()
    {
        if (_index < 0 || _index >= _stack.Size())
            return;

        const auto& state = _stack[_index];
        if (state.type == UndoState::ATTRIBUTE_CHANGED)
        {
            // Saved state is current state of item saved for redo. Step back.
            if (_index > 0 && state.item->GetAttribute(state.attribute_name) == state.attribute_value)
                _index--;
        }
        ApplyState(_stack[_index], false);
        if (_index > 0)
            _index--;
    }

    void Redo()
    {
        if (_index < 0 || _index >= _stack.Size())
            return;

        const auto& state = _stack[_index];
        if (state.type == UndoState::ATTRIBUTE_CHANGED)
        {
            // Saved state is current state of item saved for redo. Step forward.
            if (_index < _stack.Size()-1 && state.item->GetAttribute(state.attribute_name) == state.attribute_value)
                _index++;
        }
        ApplyState(_stack[_index], true);
        if (_index < _stack.Size()-1)
            _index++;
    }

    void TrackValue(Serializable* item, const String& name, const Variant& value)
    {
        if (_index >= 0 && _index < _stack.Size() && _stack.Size() > 0)
        {
            if (_stack[_index].attribute_value == value)
            {
                context_->GetLog()->Write(LOG_DEBUG, "UNDO: Same value is already at the top of undo stack. Ignore.");
                return;
            }
        }
        UndoState state;
        state.item = item;
        if (state.item.NotNull())
        {
            state.type = UndoState::ATTRIBUTE_CHANGED;
            state.attribute_name = name;
            state.attribute_value = value;
            _stack.Resize(++_index);
            _stack.Push(state);
            context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Save %d %s = %s", _index, name.CString(),
                                                          value.ToString().CString()));
        }
    }

    void TrackRemoval(UIElement* item)
    {
        TrackAddRemove(item, UndoState::UI_REMOVE);
    }

    void TrackAddition(UIElement* item)
    {
        TrackAddRemove(item, UndoState::UI_ADD);
    }

    void ApplyState(const UndoState& state, bool redo)
    {
        switch (state.type)
        {
        case UndoState::UI_ADD:
        case UndoState::UI_REMOVE:
            if ((state.type == UndoState::UI_ADD) ^ redo)
            {
                DynamicCast<UIElement>(state.parent)->RemoveChild(DynamicCast<UIElement>(state.item));
                context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Add item state %d (%s)", _index,
                                                              redo ? "redo" : "undo"));
            }
            else
            {
                DynamicCast<UIElement>(state.parent)->InsertChild(state.index, DynamicCast<UIElement>(state.item));
                context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Del item state %d (%s)", _index,
                                                              redo ? "redo" : "undo"));
            }
            break;
        case UndoState::ATTRIBUTE_CHANGED:
        {
            String repr;

            switch (state.attribute_value.GetType())
            {
            case VAR_STRINGVECTOR:
                for (auto val: state.attribute_value.GetStringVector())
                    repr += val + ";";
                break;
            default:
                repr = state.attribute_value.ToString();
            }

            state.item->SetAttribute(state.attribute_name, state.attribute_value);
            state.item->ApplyAttributes();
            context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Set state %d %s = %s",
                                                          _index, state.attribute_name.CString(), repr.CString()));
            break;
        }
        default:
            break;
        }
    }

protected:

    void TrackAddRemove(UIElement* item, UndoState::Type type)
    {
        UndoState state;
        state.type = type;
        state.item = item;
        state.parent = item->GetParent();
        state.index = DynamicCast<UIElement>(state.parent)->GetChildren().IndexOf(SharedPtr<UIElement>(item));
        _stack.Resize(++_index);
        _stack.Push(state);
        context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Track item state %d (%s)", _index,
                                                      type == UndoState::UI_ADD ? "add" : "del"));
    }

    Vector<UndoState> _stack;
    int32_t _index = -1;

};