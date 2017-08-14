#include <Atomic/Engine/Application.h>
#include <Atomic/Engine/EngineDefs.h>
#include <Atomic/Graphics/GraphicsDefs.h>
#include <Atomic/IO/FileSystem.h>
#include <Atomic/Resource/ResourceCache.h>
#include <Atomic/UI/SystemUI/SystemUI.h>
#include <Atomic/Graphics/Graphics.h>
#include <Atomic/Graphics/Octree.h>
#include <Atomic/Graphics/Zone.h>
#include <Atomic/Graphics/Renderer.h>
#include <Atomic/Graphics/Camera.h>
#include <Atomic/Graphics/DebugRenderer.h>
#include <Atomic/Scene/Scene.h>
#include <Atomic/Input/Input.h>
#include <Atomic/IO/Log.h>
#include <Atomic/Graphics/GraphicsEvents.h>
#include <Atomic/Core/CoreEvents.h>

#include <UrhoUI.h>
#include <unordered_map>
#include <tinyfiledialogs.h>
#include "IconsFontAwesome.h"
#include "UndoManager.hpp"


using namespace std::placeholders;
using namespace Atomic;
using namespace Atomic::UrhoUI;
namespace ui=ImGui;


enum ResizeType
{
    RESIZE_NONE = 0,
    RESIZE_LEFT = 1,
    RESIZE_RIGHT = 2,
    RESIZE_TOP = 4,
    RESIZE_BOTTOM = 8,
    RESIZE_MOVE = 16,
};

inline ResizeType operator|(ResizeType a, ResizeType b)
{
    return static_cast<ResizeType>(static_cast<int>(a) | static_cast<int>(b));
}

class UIEditorApplication : public Application
{
    ATOMIC_OBJECT(UIEditorApplication, Application);
public:
    SharedPtr<Scene> _scene;
    WeakPtr<UrhoUI::UI> _ui;
    WeakPtr<UIElement> _selected;
    WeakPtr<DebugRenderer> _debug;
    WeakPtr<Camera> _camera;
    HashMap<String, std::array<char, 0x1000>> _buffers;
    UndoManager _undo;
    String _current_file_path;
    bool _is_editing_value = false;
    bool _show_internal = false;
    bool _clear_buffers = true;
    ResizeType _resizing = RESIZE_NONE;
    std::array<char, 0x100> _filter;

    explicit UIEditorApplication(Context* ctx) : Application(ctx), _undo(ctx)
    {
    }

    void Setup() override
    {
        engineParameters_[EP_WINDOW_TITLE] = GetTypeName();
        engineParameters_[EP_HEADLESS] = false;
        engineParameters_[EP_RESOURCE_PATHS] = "CoreData;UIEditorData";
        engineParameters_[EP_RESOURCE_PREFIX_PATHS] = context_->GetFileSystem()->GetProgramDir();
        engineParameters_[EP_FULL_SCREEN] = false;
        engineParameters_[EP_WINDOW_HEIGHT] = 1080;
        engineParameters_[EP_WINDOW_WIDTH] = 1920;
        engineParameters_[EP_LOG_LEVEL] = LOG_DEBUG;
    }

    void Start() override
    {
        context_->RegisterFactory<UrhoUI::UI>();
        context_->RegisterSubsystem(context_->CreateObject<UrhoUI::UI>());
        _ui = GetSubsystem<UrhoUI::UI>();
        GetSubsystem<SystemUI>()->AddFont("Fonts/fontawesome-webfont.ttf", 0, {ICON_MIN_FA, ICON_MAX_FA, 0}, true);

        // UI style
        ui::GetStyle().WindowRounding = 3;

        // Background color
        _scene = new Scene(context_);
        _scene->CreateComponent<Octree>();
        _debug = _scene->CreateComponent<DebugRenderer>();
        auto zone = _scene->CreateComponent<Zone>();
        zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
        zone->SetFogColor(Color(0.1f, 0.1f, 0.1f));
        zone->SetFogStart(100.0f);
        zone->SetFogEnd(300.0f);
        _camera = _scene->CreateChild("Camera")->CreateComponent<Camera>();
        _camera->SetOrthographic(true);
        _camera->GetNode()->SetPosition({0, 10, 0});
        _camera->GetNode()->LookAt({0, 0, 0});
        _debug->SetView(_camera);
        GetSubsystem<Renderer>()->SetViewport(0, new Viewport(context_, _scene, _camera));

        // Events
        SubscribeToEvent(E_UPDATE, std::bind(&UIEditorApplication::OnUpdate, this, _2));
        SubscribeToEvent(E_SYSTEMUIFRAME, std::bind(&UIEditorApplication::RenderSystemUI, this));
        SubscribeToEvent(E_DROPFILE, std::bind(&UIEditorApplication::OnFileDrop, this, _2));

        // Arguments
        if (GetArguments().Size() > 0)
            LoadFile(GetArguments().At(0));
    }

    void Stop() override
    {
    }

    Vector3 ScreenToWorld(IntVector2 screen_pos)
    {
        auto renderer = GetSubsystem<Renderer>();
        return renderer->GetViewport(0)->GetScreenRay(screen_pos.x_, screen_pos.y_).origin_;
    }

    bool RenderHandle(IntVector2 pos)
    {
        auto wh = 10;
        IntRect rect(
            pos.x_ - wh / 2,
            pos.y_ - wh / 2,
            pos.x_ + wh / 2,
            pos.y_ + wh / 2
        );

        auto a = ScreenToWorld({rect.left_, rect.top_});
        auto b = ScreenToWorld({rect.right_, rect.top_});
        auto c = ScreenToWorld({rect.right_, rect.bottom_});
        auto d = ScreenToWorld({rect.left_, rect.bottom_});
        _debug->AddTriangle(a, b, c, Color::RED, false);
        _debug->AddTriangle(a, c, d, Color::RED, false);

        auto input = context_->GetInput();
        if (input->GetMouseButtonDown(MOUSEB_LEFT))
            return rect.IsInside(input->GetMousePosition()) == INSIDE;
        return false;
    }

    void OnUpdate(VariantMap& args)
    {
        if (_selected.Null())
            return;

        auto pos = _selected->GetScreenPosition();
        auto size = _selected->GetSize();
        auto input = context_->GetInput();

        bool was_not_moving = _resizing == RESIZE_NONE;

        if (RenderHandle(pos) && was_not_moving)
            _resizing = RESIZE_LEFT | RESIZE_TOP;
        if (RenderHandle(pos + IntVector2(0, size.y_ / 2)) && was_not_moving)
            _resizing = RESIZE_LEFT;
        if (RenderHandle(pos + IntVector2(0, size.y_)) && was_not_moving)
            _resizing = RESIZE_LEFT | RESIZE_BOTTOM;
        if (RenderHandle(pos + IntVector2(size.x_ / 2, 0)) && was_not_moving)
            _resizing = RESIZE_TOP;
        if (RenderHandle(pos + IntVector2(size.x_, 0)) && was_not_moving)
            _resizing = RESIZE_TOP | RESIZE_RIGHT;
        if (RenderHandle(pos + IntVector2(size.x_, size.y_ / 2)) && was_not_moving)
            _resizing = RESIZE_RIGHT;
        if (RenderHandle(pos + IntVector2(size.x_, size.y_)) && was_not_moving)
            _resizing = RESIZE_BOTTOM | RESIZE_RIGHT;
        if (RenderHandle(pos + IntVector2(size.x_ / 2, size.y_)) && was_not_moving)
            _resizing = RESIZE_BOTTOM;
        if (RenderHandle(pos + size / 2) && was_not_moving)
            _resizing = RESIZE_MOVE;

        if (was_not_moving && _resizing != RESIZE_NONE)
            _undo.TrackValue(_selected, {{"Position", _selected->GetPosition()}, {"Size", _selected->GetSize()}});

        if (_resizing != RESIZE_NONE && !input->GetMouseButtonDown(MOUSEB_LEFT))
        {
            _undo.TrackValue(_selected, {{"Position", _selected->GetPosition()}, {"Size", _selected->GetSize()}});
            _resizing = RESIZE_NONE;
        }

        auto d = input->GetMouseMove();
        if (_resizing != RESIZE_NONE && d != IntVector2::ZERO)
        {
            pos = _selected->GetPosition();
            if (_resizing & RESIZE_MOVE)
                pos += d;
            else
            {
                if (_resizing & RESIZE_LEFT)
                {
                    pos += IntVector2(d.x_, 0);
                    size -= IntVector2(d.x_, 0);
                }
                else if (_resizing & RESIZE_RIGHT)
                    size += IntVector2(d.x_, 0);

                if (_resizing & RESIZE_TOP)
                {
                    pos += IntVector2(0, d.y_);
                    size -= IntVector2(0, d.y_);
                }
                else if (_resizing & RESIZE_BOTTOM)
                    size += IntVector2(0, d.y_);
            }

            _selected->SetPosition(pos);
            _selected->SetSize(size);
        }
    }

    void RenderSystemUI()
    {
        _ui->Render(true);

        if (_selected.NotNull())
            _ui->DebugDraw(_selected);

        if (ui::BeginMainMenuBar())
        {
            if (ui::BeginMenu("File"))
            {
                if (ui::MenuItem(ICON_FA_FOLDER_OPEN " Open"))
                {
                    const char* filters[] = {"*.xml"};
                    auto filename = tinyfd_openFileDialog("Open file", ".", 2, filters, "XML files", 0);
                    if (filename)
                        LoadFile(filename);
                }

                if (ui::MenuItem(ICON_FA_FLOPPY_O " Save As") && _ui->GetRoot()->GetNumChildren() > 0)
                {
                    const char* filters[] = {"*.xml"};
                    if (auto path = tinyfd_saveFileDialog("Save file", ".", 1, filters, "XML files"))
                        SaveFile(path);
                }

                ui::EndMenu();
            }

            if (ui::Button(ICON_FA_FLOPPY_O) && !_current_file_path.Empty())
                SaveFile(_current_file_path);
            if (ui::IsItemHovered())
                ui::SetTooltip("Save current file.");
            ui::SameLine();

            if (ui::Button(ICON_FA_UNDO))
            {
                _undo.Undo();
                _clear_buffers = true;
            }
            if (ui::IsItemHovered())
                ui::SetTooltip("Undo.");
            ui::SameLine();

            if (ui::Button(ICON_FA_REPEAT))
            {
                _undo.Redo();
                _clear_buffers = true;
            }
            if (ui::IsItemHovered())
                ui::SetTooltip("Redo.");
            ui::SameLine();

            ui::Checkbox("Show Internal", &_show_internal);
            ui::SameLine();

            ui::EndMainMenuBar();
        }

        auto window_height = (float)context_->GetGraphics()->GetHeight();
        auto window_width = (float)context_->GetGraphics()->GetWidth();
        IntVector2 root_pos(0, 20);
        IntVector2 root_size(0, static_cast<int>(window_height) - 20);
        const auto panel_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
        
        ui::SetNextWindowPos({0.f, 20.f}, ImGuiSetCond_Once);
        ui::SetNextWindowSize({300.f, window_height - 20.f});
        if (ui::Begin("ElementTree", nullptr, panel_flags))
        {
            root_pos.x_ = static_cast<int>(ui::GetWindowWidth());
            RenderUITree(_ui->GetRoot());
        }
        ui::End();
        

        ui::SetNextWindowPos({window_width - 400.f, 20.f}, ImGuiSetCond_Once);
        ui::SetNextWindowSize({400.f, window_height - 20.f});
        if (ui::Begin("AttributeList", nullptr, panel_flags))
        {
            root_size.x_ = static_cast<int>(window_width - root_pos.x_ - ui::GetWindowWidth());
            if (_selected)
                RenderAttributes(_selected);
        }
        ui::End();
        
        _ui->GetRoot()->SetSize(root_size);
        _ui->GetRoot()->SetPosition(root_pos);

        auto input = context_->GetInput();
        if (input->GetMouseButtonPress(MOUSEB_LEFT) || input->GetMouseButtonPress(MOUSEB_RIGHT))
        {
            auto clicked = _ui->GetElementAt(input->GetMousePosition(), false);
            if (clicked)
                SelectItem(clicked);
        }

        if (_selected)
        {
            if (input->GetKeyPress(KEY_DELETE) && _selected != _ui->GetRoot())
            {
                _undo.TrackRemoval(_selected);
                _selected->Remove();
                SelectItem(nullptr);
            }

            if (ui::BeginPopupContextVoid("Element Context Menu", 2))
            {
                if (ui::BeginMenu("Add Child"))
                {
                    const char* ui_types[] = {"BorderImage", "Button", "CheckBox", "Cursor", "DropDownList", "LineEdit",
                        "ListView", "Menu", "ProgressBar", "ScrollBar", "ScrollView", "Slider", "Sprite", "Text",
                        "ToolTip", "UIElement", "View3D", "Window", 0
                    };
                    for (auto i = 0; ui_types[i] != 0; i++)
                    {
                        if (ui::MenuItem(ui_types[i]))
                        {
                            SelectItem(_selected->CreateChild(ui_types[i]));
                            _selected->SetStyleAuto();
                            _undo.TrackAddition(_selected);
                        }
                    }
                    ui::EndMenu();
                }

                if (_selected != _ui->GetRoot())
                {
                    if (ui::MenuItem("Delete Element"))
                    {
                        _undo.TrackRemoval(_selected);
                        _selected->Remove();
                        SelectItem(nullptr);
                    }

                    if (ui::MenuItem("Bring To Front"))
                        _selected->BringToFront();
                }
                ui::EndPopup();
            }
        }

        _clear_buffers = false;
        if (!ui::IsAnyItemActive())
        {
            if (input->GetKeyDown(KEY_CTRL))
            {
                if (input->GetKeyPress(KEY_Y) || (input->GetKeyDown(KEY_SHIFT) && input->GetKeyPress(KEY_Z)))
                {
                    _undo.Redo();
                    _clear_buffers = true;
                }
                else if (input->GetKeyPress(KEY_Z))
                {
                    _undo.Undo();
                    _clear_buffers = true;
                }
            }
        }
    }

    void OnFileDrop(VariantMap& args)
    {
        LoadFile(args[DropFile::P_FILENAME].GetString());
    }

    String GetResourcePath(String file_path)
    {
        auto pos = file_path.FindLast('/');
        file_path.Erase(pos, file_path.Length() - pos);
        pos = file_path.FindLast('/');
        file_path.Erase(pos, file_path.Length() - pos);
        return file_path;
    }

    bool LoadFile(const String& file_path)
    {
        auto cache = GetSubsystem<ResourceCache>();
        if (!_current_file_path.Empty())
            cache->RemoveResourceDir(GetResourcePath(_current_file_path));

        auto resource_dir = GetResourcePath(file_path);
        if (!cache->GetResourceDirs().Contains(resource_dir))
            cache->AddResourceDir(resource_dir);

        if (file_path.EndsWith(".xml", false))
        {
            SharedPtr<XMLFile> xml(new XMLFile(context_));
            if (xml->LoadFile(file_path))
            {
                if (xml->GetRoot().GetName() == "elements")
                {
                    // This is a style.
                    _ui->GetRoot()->SetDefaultStyle(xml);
                    return true;
                }
                else if (xml->GetRoot().GetName() == "element")
                {
                    Vector<SharedPtr<UIElement>> children = _ui->GetRoot()->GetChildren();
                    auto child = _ui->GetRoot()->CreateChild<UIElement>();
                    if (child->LoadXML(xml->GetRoot()))
                    {
                        child->SetStyleAuto();
                        SetCurrentFilePath(file_path);

                        for (auto old_child : children)
                            old_child->Remove();

                        return true;
                    }
                    else
                        child->Remove();
                }
            }
        }

        cache->RemoveResourceDir(resource_dir);
        tinyfd_messageBox("Error", "Opening XML file failed", "ok", "error", 1);
        return false;
    }

    bool SaveFile(const String& file_path)
    {
        File saveFile(context_, file_path, FILE_WRITE);
        if (file_path.EndsWith(".xml", false))
        {
            if (_ui->GetRoot()->GetChild(0)->SaveXML(saveFile))
            {
                SetCurrentFilePath(file_path);
                return true;
            }
        }

        tinyfd_messageBox("Error", "Saving UI file failed", "ok", "error", 1);
        return true;
    }

    void RenderUITree(UIElement* element)
    {
        auto& name = element->GetName();
        auto& type = element->GetTypeName();
        auto tooltip = "Type: " + type;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        bool is_internal = element->IsInternal();
        if (is_internal && !_show_internal)
            return;
        else
            flags |= ImGuiTreeNodeFlags_DefaultOpen;

        if (_show_internal)
            tooltip += String("\nInternal: ") + (is_internal ? "true" : "false");

        if (element == _selected)
            flags |= ImGuiTreeNodeFlags_Selected;

        if (ui::TreeNodeEx(element, flags, "%s", name.Length() ? name.CString() : type.CString()))
        {
            if (ui::IsItemHovered())
                ui::SetTooltip(tooltip.CString());

            if (ui::IsItemHovered() && ui::IsMouseClicked(0))
                SelectItem(element);

            for (auto child: element->GetChildren())
                RenderUITree(child);
            ui::TreePop();
        }
    }

    void RenderAttributes(Serializable* item)
    {
        ui::Columns(2);

        ui::TextUnformatted("Filter");
        ui::NextColumn();
        if (ui::Button(ICON_FA_UNDO))
            _filter.front() = 0;
        if (ui::IsItemHovered())
            ui::SetTooltip("Reset filter.");
        ui::SameLine();
        ui::InputText("", &_filter.front(), _filter.size() - 1);
        ui::NextColumn();

        ui::PushID(item);
        const auto& attributes = *item->GetAttributes();
        for (const AttributeInfo& info: attributes)
        {
            if (info.mode_ & AM_NOEDIT)
                continue;

            if (_filter.front() && !info.name_.Contains(&_filter.front(), false))
                continue;

            Variant value, old_value;
            value = old_value = item->GetAttribute(info.name_);

            bool modified = false;

            const int int_min = M_MIN_INT;
            const int int_max = M_MAX_INT;
            const int int_step = 1;
            const float float_min = -14000.f;
            const float float_max = 14000.f;
            const float float_step = 0.01f;

            const char** combo_values = 0;
            auto combo_values_num = 0;

            if (info.name_ == "Text Alignment")
            {
                static const char* values[] = {"Left", "Center", "Right"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Text Effect")
            {
                static const char* values[] = {"None", "Shadow", "Stroke"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Collision Event Mode")
            {
                static const char* values[] = {"Never", "Active", "Always"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Focus Mode")
            {
                static const char* values[] = {"Not focusable", "Reset focus", "Focusable", "Focusable/Defocusable"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Drag And Drop Mode")
            {
                static const char* values[] = {"Disabled", "Source", "Target", "Source/Target"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Layout Mode")
            {
                static const char* values[] = {"Free", "Horizontal", "Vertical"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Blend Mode")
            {
                static const char* values[] = {"Replace", "Add", "Multiply", "Alpha", "AddAlpha",
                                               "PremulAlpha", "InvDestAlpha", "Subtract", "SubtractAlpha"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Loop Mode")
            {
                static const char* values[] = {"Default", "Force Lopped", "Force Clamped"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Face Camera Mode")
            {
                static const char* values[] = {"None", "Rotate XYZ", "Rotate Y", "Lookat XYZ", "Lookat Y",
                                               "Direction"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Horiz Alignment")
            {
                static const char* values[] = {"Left", "Center", "Right"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }
            else if (info.name_ == "Vert Alignment")
            {
                static const char* values[] = {"Top", "Center", "Bottom"};
                combo_values = values;
                combo_values_num = SDL_arraysize(values);
            }

            ui::PushID(info.name_.CString());
            ui::TextUnformatted(info.name_.CString());
            ui::NextColumn();

            if (ui::Button(ICON_FA_UNDO))
            {
                _undo.TrackValue(item, info.name_, value);
                item->SetAttribute(info.name_, info.defaultValue_);
                item->ApplyAttributes();
                _undo.TrackValue(item, info.name_, info.defaultValue_);
            }
            if (ui::IsItemActive())
                ui::SetTooltip("Set default value.");
            ui::SameLine();

            if (combo_values)
            {
                int current = value.GetInt();
                modified |= ui::Combo("", &current, combo_values, combo_values_num);
                if (modified)
                    value = current;
            }
            else
            {
                switch (info.type_)
                {
                case VAR_NONE:
                    ui::TextUnformatted("None");
                    break;
                case VAR_INT:
                {
                    // TODO: replace this with custom control that properly handles int types.
                    auto v = value.GetInt();
                    modified |= ui::DragInt("", &v, int_step, int_min, int_max);
                    if (modified)
                        value = v;
                    break;
                }
                case VAR_BOOL:
                {
                    auto v = value.GetBool();
                    modified |= ui::Checkbox("", &v);
                    if (modified)
                        value = v;
                    break;
                }
                case VAR_FLOAT:
                {
                    auto v = value.GetFloat();
                    modified |= ui::DragFloat("", &v, float_step, float_min, float_max);
                    if (modified)
                        value = v;
                    break;
                }
                case VAR_VECTOR2:
                {
                    auto& v = value.GetVector2();
                    modified |= ui::DragFloat2("xy", const_cast<float*>(&v.x_), float_step, float_min, float_max);
                    break;
                }
                case VAR_VECTOR3:
                {
                    auto& v = value.GetVector3();
                    modified |= ui::DragFloat3("xyz", const_cast<float*>(&v.x_), float_step, float_min, float_max);
                    break;
                }
                case VAR_VECTOR4:
                {
                    auto& v = value.GetVector4();
                    modified |= ui::DragFloat4("xyzw", const_cast<float*>(&v.x_), float_step, float_min, float_max);
                    break;
                }
                case VAR_QUATERNION:
                {
                    auto& v = value.GetQuaternion();
                    modified |= ui::DragFloat4("wxyz", const_cast<float*>(&v.w_), float_step, float_min, float_max);
                    break;
                }
                case VAR_COLOR:
                {
                    auto& v = value.GetColor();
                    modified |= ui::ColorEdit4("rgba", const_cast<float*>(&v.r_));
                    break;
                }
                case VAR_STRING:
                {
                    auto& v = const_cast<String&>(value.GetString());
                    auto& buffer = GetBuffer(info.name_, value.GetString());
                    modified |= ui::InputText("", &buffer.front(), buffer.size() - 1);
                    if (modified)
                        value = &buffer.front();
                    break;
                }
//            case VAR_BUFFER:
                case VAR_VOIDPTR:
                    ui::Text("%p", value.GetVoidPtr());
                    break;
                case VAR_RESOURCEREF:
                {
                    auto ref = value.GetResourceRef();
                    ui::Text("%s", ref.name_.CString());
                    ui::SameLine();
                    if (ui::Button(ICON_FA_FOLDER_OPEN))
                    {
                        auto cache = GetSubsystem<ResourceCache>();
                        auto file_name = cache->GetResourceFileName(ref.name_);
                        String selected_path = tinyfd_openFileDialog(
                            ToString("Open %s File", context_->GetTypeName(ref.type_).CString()).CString(),
                            file_name.Length() ? file_name.CString() : _current_file_path.CString(), 0, 0, 0, 0);
                        SharedPtr<Resource> resource(cache->GetResource(ref.type_, selected_path));
                        if (resource.NotNull())
                        {
                            ref.name_ = resource->GetName();
                            value = ref;
                            modified = true;
                        }
                    }
                    break;
                }
//            case VAR_RESOURCEREFLIST:
//            case VAR_VARIANTVECTOR:
//            case VAR_VARIANTMAP:
                case VAR_INTRECT:
                {
                    auto& v = value.GetIntRect();
                    modified |= ui::DragInt4("ltbr", const_cast<int*>(&v.left_), int_step, int_min, int_max);
                    break;
                }
                case VAR_INTVECTOR2:
                {
                    auto& v = value.GetIntVector2();
                    modified |= ui::DragInt2("xy", const_cast<int*>(&v.x_), int_step, int_min, int_max);
                    break;
                }
                case VAR_PTR:
                    ui::Text("%p (%s)", value.GetPtr(), value.GetPtr()->GetTypeName().CString());
                    break;
                case VAR_MATRIX3:
                {
                    auto& v = value.GetMatrix3();
                    modified |= ui::DragFloat3("m0", const_cast<float*>(&v.m00_), float_step, float_min, float_max);
                    modified |= ui::DragFloat3("m1", const_cast<float*>(&v.m10_), float_step, float_min, float_max);
                    modified |= ui::DragFloat3("m2", const_cast<float*>(&v.m20_), float_step, float_min, float_max);
                    break;
                }
                case VAR_MATRIX3X4:
                {
                    auto& v = value.GetMatrix3x4();
                    modified |= ui::DragFloat4("m0", const_cast<float*>(&v.m00_), float_step, float_min, float_max);
                    modified |= ui::DragFloat4("m1", const_cast<float*>(&v.m10_), float_step, float_min, float_max);
                    modified |= ui::DragFloat4("m2", const_cast<float*>(&v.m20_), float_step, float_min, float_max);
                    break;
                }
                case VAR_MATRIX4:
                {
                    auto& v = value.GetMatrix4();
                    modified |= ui::DragFloat4("m0", const_cast<float*>(&v.m00_), float_step, float_min, float_max);
                    modified |= ui::DragFloat4("m1", const_cast<float*>(&v.m10_), float_step, float_min, float_max);
                    modified |= ui::DragFloat4("m2", const_cast<float*>(&v.m20_), float_step, float_min, float_max);
                    modified |= ui::DragFloat4("m3", const_cast<float*>(&v.m30_), float_step, float_min, float_max);
                    break;
                }
                case VAR_DOUBLE:
                {
                    // TODO: replace this with custom control that properly handles double types.
                    float v = value.GetDouble();
                    modified |= ui::DragFloat("", &v, float_step, float_min, float_max);
                    if (modified)
                        value = (double)v;
                    break;
                }
                case VAR_STRINGVECTOR:
                {
                    auto index = 0;
                    auto& v = const_cast<StringVector&>(value.GetStringVector());

                    // Insert new item.
                    {
                        auto& buffer = GetBuffer(info.name_, "");
                        ui::PushID(index++);
                        if (ui::InputText("", &buffer.front(), buffer.size() - 1, ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            v.Push(&buffer.front());
                            buffer.front() = 0;
                            modified = true;
                        }
                        ui::PopID();
                    }

                    // List of current items.
                    for (String& sv: v)
                    {
                        auto buffer_name = ToString("%s-%d", info.name_.CString(), index);
                        if (_clear_buffers)
                            RemoveBuffer(buffer_name);
                        auto& buffer = GetBuffer(buffer_name, sv);
                        ui::PushID(index++);
                        if (ui::Button(ICON_FA_TRASH))
                        {
                            RemoveBuffer(buffer_name);
                            v.Remove(sv);
                            modified = true;
                            ui::PopID();
                            break;
                        }
                        ui::SameLine();

                        modified |= ui::InputText("", &buffer.front(), buffer.size() - 1, ImGuiInputTextFlags_EnterReturnsTrue);
                        if (modified)
                            sv = &buffer.front();
                        ui::PopID();
                    }

                    if (modified)
                        value = StringVector(v);

                    break;
                }
                case VAR_RECT:
                {
                    auto& v = value.GetRect();
                    modified |= ui::DragFloat2("min xy", const_cast<float*>(&v.min_.x_), float_step, float_min,
                                               float_max);
                    ui::SameLine();
                    modified |= ui::DragFloat2("max xy", const_cast<float*>(&v.max_.x_), float_step, float_min,
                                               float_max);
                    break;
                }
                case VAR_INTVECTOR3:
                {
                    auto& v = value.GetIntVector3();
                    modified |= ui::DragInt3("xyz", const_cast<int*>(&v.x_), int_step, int_min, int_max);
                    break;
                }
                case VAR_INT64:
                {
                    // TODO: replace this with custom control that properly handles int types.
                    int v = value.GetInt64();
                    modified |= ui::DragInt("", &v, int_step, int_min, int_max, "%d");
                    if (modified)
                        value = (long long)v;
                    break;
                }
                default:
                    ui::TextUnformatted("Unhandled attribute type.");
                    break;
                }
            }

            if (modified)
            {
                if (!_is_editing_value)
                {
                    _is_editing_value = true;
                    _undo.TrackValue(item, info.name_, old_value);
                }
                item->SetAttribute(info.name_, value);
                item->ApplyAttributes();
            }

            if (_is_editing_value && !ui::IsAnyItemActive())
            {
                _undo.TrackValue(item, info.name_, value);
                _is_editing_value = false;
            }

            ui::PopID();
            ui::NextColumn();
        }
        ui::PopID();
        ui::Columns(1);
    }

    void SetCurrentFilePath(const String& file_path)
    {
        _current_file_path = file_path;
        context_->GetGraphics()->SetWindowTitle("UrhoUIEditor - " + _current_file_path);
    }

    void SelectItem(UIElement* current)
    {
        if (_resizing)
            return;

        _buffers.Clear();
        _selected = current;
    }

    std::array<char, 0x1000>& GetBuffer(const String& name, const String& default_value)
    {
        auto it = _buffers.Find(name);
        if (it == _buffers.End())
        {
            auto& buffer = _buffers[name];
            strncpy(&buffer[0], default_value.CString(), buffer.size() - 1);
            return buffer;
        }
        else
            return it->second_;
    }

    void RemoveBuffer(const String& name)
    {
        _buffers.Erase(name);
    }
};

ATOMIC_DEFINE_APPLICATION_MAIN(UIEditorApplication);
