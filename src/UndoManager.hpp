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
        INVALID_STATE,
        ATTRIBUTE_CHANGED,
        UI_ADD,
        UI_REMOVE,
    } type = INVALID_STATE;

    /// Object that was modified.
    SharedPtr<Serializable> item;
    /// Changed attributes.
    HashMap<String, Variant> attributes;

    /// Parent of `element`.
    SharedPtr<Serializable> parent;
    /// Index of `element` in children list of `parent`.
    unsigned index;


    bool operator==(const UndoState& other) const
    {
        if (type != other.type || item != other.item)
            return false;

        switch (type)
        {
        case ATTRIBUTE_CHANGED:
        {
            if (attributes.Size() != other.attributes.Size())
                return false;

            for (auto it: attributes)
            {
                auto jt = other.attributes.Find(it.first_);
                if (jt == other.attributes.End())
                    return false;
                if (jt->second_ != it.second_)
                    return false;
            }
            return true;
        }
        case UI_ADD:
        case UI_REMOVE:
            return index == other.index && parent == other.parent;
        default:
            return false;
        }
    }

    bool Equals(const Serializable* other_item) const
    {
        if (item != other_item)
            return false;

        switch (type)
        {
        case ATTRIBUTE_CHANGED:
        {
            for (auto it: attributes)
            {
                if (other_item->GetAttribute(it.first_) != it.second_)
                    return false;
            }
            return true;
        }
        case UI_ADD:
            return DynamicCast<UIElement>(parent)->GetChild(index) == other_item;
        case UI_REMOVE:
            return DynamicCast<UIElement>(parent)->GetChild(index) != other_item;
        default:
            return false;
        }
    }
};


class UndoManager : public Object
{
    ATOMIC_OBJECT(UndoManager, Object);
public:
    UndoManager(Context* ctx) : Object(ctx) { }

    void Undo()
    {
        while (_index >= 0 && _index < _stack.Size() && !ApplyState(false))
            _index--;
        _index--;
        _index = Clamp<int32_t>(_index, 0, _stack.Size()-1);
    }

    void Redo()
    {
        while (_index >= 0 && _index < _stack.Size() && !ApplyState(false))
            _index++;
        _index++;
        _index = Clamp<int32_t>(_index, 0, _stack.Size()-1);
    }

    void TrackValue(Serializable* item, const String& name, const Variant& value)
    {
        UndoState state;
        state.item = item;
        if (state.item.NotNull())
        {
            state.type = UndoState::ATTRIBUTE_CHANGED;
            state.attributes[name] = value;
            _stack.Resize(++_index);

            while (_stack.Size() > 0 && _stack.Back() == state)
            {
                context_->GetLog()->Write(LOG_DEBUG, "UNDO: Same value is already at the top of undo stack. Ignore.");
                return;
            }

            _stack.Push(state);
            context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Save %d %s = %s", _index, name.CString(),
                                                          value.ToString().CString()));
        }
    }

    void TrackValue(Serializable* item, const HashMap<String, Variant>& values)
    {
        UndoState state;
        state.item = item;
        if (state.item.NotNull())
        {
            state.type = UndoState::ATTRIBUTE_CHANGED;
            state.attributes = values;
            _stack.Resize(++_index);

            if (_stack.Size() > 0 && _stack.Back() == state)
            {
                context_->GetLog()->Write(LOG_DEBUG, "UNDO: Same value is already at the top of undo stack. Ignore.");
                return;
            }

            _stack.Push(state);
            context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Save %d", _index));
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

    bool ApplyState(bool redo)
    {
        const UndoState& state = _stack[_index];
        bool modified = false;
        switch (state.type)
        {
        case UndoState::UI_ADD:
        case UndoState::UI_REMOVE:
        {
            auto el = DynamicCast<UIElement>(state.item);
            auto parent = DynamicCast<UIElement>(state.parent);
            if ((state.type == UndoState::UI_ADD) ^ redo)
            {
                if (parent->GetChildren().Contains(el))
                {
                    DynamicCast<UIElement>(state.parent)->RemoveChild(el);
                    context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Add item state %d (%s)", _index,
                                                                  redo ? "redo" : "undo"));
                    modified = true;
                }
                else
                    context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Skip state %d", _index));
            }
            else
            {
                if (!parent->GetChildren().Contains(el))
                {
                    DynamicCast<UIElement>(state.parent)->InsertChild(state.index, el);
                    context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Del item state %d (%s)", _index,
                                                                  redo ? "redo" : "undo"));
                    modified = true;
                }
                else
                    context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Skip state %d", _index));
            }
            break;
        }
        case UndoState::ATTRIBUTE_CHANGED:
        {
            for (auto it: state.attributes)
            {
                if (state.item->GetAttribute(it.first_) != it.second_)
                {
                    state.item->SetAttribute(it.first_, it.second_);
                    modified = true;
                }
            }
            if (modified)
            {
                state.item->ApplyAttributes();
                context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Set state %d", _index));
            }
            else
                context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Skip state %d", _index));
            break;
        }
        default:
            break;
        }

        return modified;
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

        if (_stack.Size() > 0 && _stack.Back() == state)
        {
            context_->GetLog()->Write(LOG_DEBUG, "UNDO: Same value is already at the top of undo stack. Ignore.");
            return;
        }

        _stack.Push(state);
        context_->GetLog()->Write(LOG_DEBUG, ToString("UNDO: Track item state %d (%s)", _index,
                                                      type == UndoState::UI_ADD ? "add" : "del"));
    }

    Vector<UndoState> _stack;
    int32_t _index = -1;

};