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
    } type;

    SharedPtr<Serializable> element;
    String attribute_name;
    Variant attribute_value;
};


class UndoManager : public Object
{
    ATOMIC_OBJECT(UndoManager, Object);
public:
    UndoManager(Context* ctx) : Object(ctx) { }

    void Undo()
    {
        assert(_undo_index >= 0 && _undo_index <= _undo.Size());
        if (_undo_index > 0 && _undo_index <= _undo.Size())
        {
            const auto& state = _undo[--_undo_index];
            auto current_value = state.element->GetAttribute(state.attribute_name);
            // Saved state is current state of item saved for redo. Step back.
            if (current_value == state.attribute_value && _undo_index > 0)
                _undo_index--;

            ApplyState(_undo[_undo_index]);
        }
    }

    void Redo()
    {
        assert(_undo_index >= 0 && _undo_index <= _undo.Size());
        if (_undo_index >= 0 && _undo_index < _undo.Size()-1)
        {
            const auto& state = _undo[++_undo_index];
            auto current_value = state.element->GetAttribute(state.attribute_name);
            // Saved state is current state of item saved for redo. Step forward.
            if (current_value == state.attribute_value && _undo_index < _undo.Size()-1)
                _undo_index++;

            ApplyState(_undo[_undo_index]);
        }
    }

    void TrackValue(Serializable* item, const String& name, const Variant& value)
    {
        assert(_undo_index >= 0 && _undo_index <= _undo.Size());

        switch (value.GetType())
        {
        case VAR_PTR:
        case VAR_VOIDPTR:
        case VAR_BUFFER:
        case VAR_VARIANTMAP:
        case VAR_STRINGVECTOR:
            context_->GetLog()->Write(LOG_DEBUG, ToString("TODO: implement undo for %s", value.GetTypeName().CString()));
            return;
        default:
        {
            if (_undo_index <= _undo.Size() && _undo.Size() > 0)
            {
                if (_undo[_undo_index-1].attribute_value == value)
                {
                    context_->GetLog()->Write(LOG_DEBUG, "Same value is already at the top of undo stack. Ignore.");
                    return;
                }
            }
            UndoState state;
            state.element = item;
            if (state.element.NotNull())
            {
                state.type = UndoState::ATTRIBUTE_CHANGED;
                state.attribute_name = name;
                state.attribute_value = value;
                _undo.Resize(_undo_index);
                context_->GetLog()->Write(LOG_DEBUG, ToString("Save %d %s = %s", _undo_index, name.CString(), value.ToString().CString()));
                _undo_index++;
                _undo.Push(state);
            }
            break;
        }
        }
    }

    void ApplyState(const UndoState& state)
    {
        assert(_undo_index >= 0 && _undo_index <= _undo.Size());
        switch (state.type)
        {
        case UndoState::ATTRIBUTE_CHANGED:
            state.element->SetAttribute(state.attribute_name, state.attribute_value);
            state.element->ApplyAttributes();
            context_->GetLog()->Write(LOG_DEBUG, ToString("Set state %d %s = %s", _undo_index, state.attribute_name.CString(), state.attribute_value.ToString().CString()));
            break;
        default:
            break;
        }
    }

    Vector<UndoState> _undo;
    int32_t _undo_index = 0;

};