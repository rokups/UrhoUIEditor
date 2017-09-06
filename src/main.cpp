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
#include <array>
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

inline ImVec4 ToImGui(const Color& color)
{
    return ImVec4(color.r_, color.g_, color.b_, color.a_);
}


inline unsigned MakeHash(const ResizeType& value)
{
    return value;
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
    String _current_style_file_path;
    bool _is_editing_value = false;
    bool _show_internal = false;
    bool _clear_buffers = true;
    ResizeType _resizing = RESIZE_NONE;
    std::array<char, 0x100> _filter;
    SharedPtr<XMLFile> _style_file;
    Vector<String> _style_names;
    HashMap<ResizeType, SDL_Cursor*> cursors;
    SDL_Cursor* cursor_arrow;
    bool _hide_resize_handles = false;

    explicit UIEditorApplication(Context* ctx)
        : Application(ctx)
        , _undo(ctx)
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
        cursors[RESIZE_MOVE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
        cursors[RESIZE_LEFT] = cursors[RESIZE_RIGHT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
        cursors[RESIZE_BOTTOM] = cursors[RESIZE_TOP] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
        cursors[RESIZE_TOP | RESIZE_LEFT] = cursors[RESIZE_BOTTOM | RESIZE_RIGHT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
        cursors[RESIZE_TOP | RESIZE_RIGHT] = cursors[RESIZE_BOTTOM | RESIZE_LEFT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);
        cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);

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
        for (auto arg: GetArguments())
            LoadFile(arg);
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
        auto wh = 8;
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

        if (!_hide_resize_handles)
        {
            _debug->AddTriangle(a, b, c, Color::RED, false);
            _debug->AddTriangle(a, c, d, Color::RED, false);
        }

        auto input = context_->GetInput();
        return rect.IsInside(input->GetMousePosition()) == INSIDE;
    }

    void OnUpdate(VariantMap& args)
    {
        if (_selected.Null() || _selected == _ui->GetRoot())
            return;

        auto pos = _selected->GetScreenPosition();
        auto size = _selected->GetSize();
        auto input = context_->GetInput();

        bool was_not_moving = _resizing == RESIZE_NONE;

        bool can_resize_horizontal = _selected->GetMinSize().x_ != _selected->GetMaxSize().x_;
        bool can_resize_vertical = _selected->GetMinSize().y_ != _selected->GetMaxSize().y_;

        ResizeType resizing = RESIZE_NONE;
        if (RenderHandle(pos + size / 2))
            resizing = RESIZE_MOVE;
        if (can_resize_horizontal && can_resize_vertical && RenderHandle(pos))
            resizing = RESIZE_LEFT | RESIZE_TOP;
        if (can_resize_horizontal && RenderHandle(pos + IntVector2(0, size.y_ / 2)))
            resizing = RESIZE_LEFT;
        if (can_resize_horizontal && can_resize_vertical && RenderHandle(pos + IntVector2(0, size.y_)))
            resizing = RESIZE_LEFT | RESIZE_BOTTOM;
        if (can_resize_vertical && RenderHandle(pos + IntVector2(size.x_ / 2, 0)))
            resizing = RESIZE_TOP;
        if (can_resize_horizontal && can_resize_vertical && RenderHandle(pos + IntVector2(size.x_, 0)))
            resizing = RESIZE_TOP | RESIZE_RIGHT;
        if (can_resize_horizontal && RenderHandle(pos + IntVector2(size.x_, size.y_ / 2)))
            resizing = RESIZE_RIGHT;
        if (can_resize_horizontal && can_resize_vertical && RenderHandle(pos + IntVector2(size.x_, size.y_)))
            resizing = RESIZE_BOTTOM | RESIZE_RIGHT;
        if (can_resize_vertical && RenderHandle(pos + IntVector2(size.x_ / 2, size.y_)))
            resizing = RESIZE_BOTTOM;

        if (resizing == RESIZE_NONE)
            SDL_SetCursor(cursor_arrow);
        else
            SDL_SetCursor(cursors[resizing]);

        if (input->GetMouseButtonDown(MOUSEB_LEFT))
        {
            // Start resizing only when resize is not in progress.
            if (was_not_moving)
                _resizing = resizing;
        }
        else
            _resizing = RESIZE_NONE;

        auto d = input->GetMouseMove();
        if (_resizing != RESIZE_NONE)
        {
            if (was_not_moving)
                _undo.TrackValue(_selected, {{"Position", _selected->GetPosition()}, {"Size", _selected->GetSize()}});

            if (!input->GetMouseButtonDown(MOUSEB_LEFT))
            {
                _undo.TrackValue(_selected, {{"Position", _selected->GetPosition()}, {"Size", _selected->GetSize()}});
                _resizing = RESIZE_NONE;
            }

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
        _debug->Render();

        if (_selected.NotNull())
            _ui->DebugDraw(_selected);

        if (ui::BeginMainMenuBar())
        {
            if (ui::BeginMenu("File"))
            {
                if (ui::MenuItem(ICON_FA_FILE_TEXT " New"))
                    _ui->GetRoot()->RemoveAllChildren();

                const char* filters[] = {"*.xml"};
                if (ui::MenuItem(ICON_FA_FOLDER_OPEN " Open"))
                {
                    auto filename = tinyfd_openFileDialog("Open file", ".", 2, filters, "XML files", 0);
                    if (filename)
                        LoadFile(filename);
                }

                if (ui::MenuItem(ICON_FA_FLOPPY_O " Save UI As") && _ui->GetRoot()->GetNumChildren() > 0)
                {
                    if (auto path = tinyfd_saveFileDialog("Save UI file", ".", 1, filters, "XML files"))
                        SaveFileUI(path);
                }

                if (ui::MenuItem(ICON_FA_FLOPPY_O " Save Style As") && _style_file.NotNull())
                {
                    if (auto path = tinyfd_saveFileDialog("Save Style file", ".", 1, filters, "XML files"))
                        SaveFileStyle(path);
                }

                ui::EndMenu();
            }

            if (ui::Button(ICON_FA_FLOPPY_O))
            {
                if (!_current_file_path.Empty())
                    SaveFileUI(_current_file_path);
                if (!_style_file.Null())
                    SaveFileStyle(_current_style_file_path);
            }

            if (ui::IsItemHovered())
                ui::SetTooltip("Save current UI and style files.");
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

            ui::Checkbox("Hide Resize Handles", &_hide_resize_handles);
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
        if (_resizing == RESIZE_NONE && input->GetMouseButtonPress(MOUSEB_LEFT) || input->GetMouseButtonPress(MOUSEB_RIGHT))
        {
            auto pos = input->GetMousePosition();
            auto clicked = _ui->GetElementAt(pos, false);
            if (!clicked && _ui->GetRoot()->GetCombinedScreenRect().IsInside(pos) == INSIDE)
                clicked = _ui->GetRoot();

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
                        // TODO: element creation with custom styles more usable.
                        if (input->GetKeyDown(KEY_SHIFT))
                        {
                            if (ui::BeginMenu(ui_types[i]))
                            {
                                for (auto j = 0; j < _style_names.Size(); j++)
                                {
                                    if (ui::MenuItem(_style_names[j].CString()))
                                    {
                                        SelectItem(_selected->CreateChild(ui_types[i]));
                                        _selected->SetStyle(_style_names[j]);
                                        _undo.TrackAddition(_selected);
                                    }
                                }
                                ui::EndMenu();
                            }
                        }
                        else
                        {
                            if (ui::MenuItem(ui_types[i]))
                            {
                                SelectItem(_selected->CreateChild(ui_types[i]));
                                _selected->SetStyleAuto();
                                _undo.TrackAddition(_selected);
                            }
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
                    _style_file = xml;
                    _current_style_file_path = file_path;

                    auto styles = _style_file->GetRoot().SelectPrepared(XPathQuery("/elements/element"));
                    for (auto i = 0; i < styles.Size(); i++)
                    {
                        auto type = styles[i].GetAttribute("type");
                        if (type.Length() && !_style_names.Contains(type))
                            _style_names.Push(type);
                    }
                    Sort(_style_names.Begin(), _style_names.End());
                    UpdateWindowTitle();
                    return true;
                }
                else if (xml->GetRoot().GetName() == "element")
                {
                    Vector<SharedPtr<UIElement>> children = _ui->GetRoot()->GetChildren();
                    auto child = _ui->GetRoot()->CreateChild<UIElement>();
                    if (child->LoadXML(xml->GetRoot()))
                    {
                        child->SetStyleAuto();
                        _current_file_path = file_path;
                        UpdateWindowTitle();

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

    bool SaveFileUI(const String& file_path)
    {
        if (file_path.EndsWith(".xml", false))
        {
            XMLFile xml(context_);
            XMLElement root = xml.CreateRoot("element");
            if (_ui->GetRoot()->GetChild(0)->SaveXML(root))
            {
                // Remove internal UI elements
                auto result = root.SelectPrepared(XPathQuery("//element[@internal=\"true\"]"));
                for (auto el = result.FirstResult(); el.NotNull(); el = el.NextResult())
                    el.GetParent().RemoveChild(el);

                // Remove style="none"
                root.SelectPrepared(XPathQuery("//element[@style=\"none\"]"));
                for (auto el = result.FirstResult(); el.NotNull(); el = el.NextResult())
                    el.RemoveAttribute("style");

                File saveFile(context_, file_path, FILE_WRITE);
                xml.Save(saveFile);

                _current_file_path = file_path;
                UpdateWindowTitle();
                return true;
            }
        }

        tinyfd_messageBox("Error", "Saving UI file failed", "ok", "error", 1);
        return false;
    }

    bool SaveFileStyle(const String& file_path)
    {
        if (file_path.EndsWith(".xml", false) && _style_file.NotNull())
        {
            File saveFile(context_, file_path, FILE_WRITE);
            _style_file->Save(saveFile);

            _current_style_file_path = file_path;
            UpdateWindowTitle();
            return true;
        }

        tinyfd_messageBox("Error", "Saving UI file failed", "ok", "error", 1);
        return false;
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

    String GetAppliedStyle(UIElement* element = nullptr)
    {
        if (element == nullptr)
            element = _selected;

        if (element == nullptr)
            return "";

        auto applied_style = _selected->GetAppliedStyle();
        if (applied_style.Empty())
            applied_style = _selected->GetTypeName();
        return applied_style;
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
        ui::PushID("FilterEdit");
        ui::InputText("", &_filter.front(), _filter.size() - 1);
        ui::PopID();
        ui::NextColumn();

        ui::TextUnformatted("Style");
        ui::NextColumn();

        auto type_style = GetAppliedStyle();
        ui::TextUnformatted(type_style.CString());

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
            if (info.enumNames_)
            {
                combo_values = info.enumNames_;
                for (; combo_values[++combo_values_num];);
            }

            ui::PushID(info.name_.CString());

            XMLElement style_attribute;
            XMLElement style_xml;
            Variant style_variant;
            GetStyleData(info, style_xml, style_attribute, style_variant);

            ImVec4 color = ToImGui(Color::WHITE);
            if (!style_variant.IsEmpty())
            {
                if (style_variant == value)
                    color = ToImGui(Color::GRAY);
                else
                    color = ToImGui(Color::GREEN);
            }

            ui::TextColored(color, "%s", info.name_.CString());
            ui::NextColumn();

            if (ui::Button(ICON_FA_CARET_DOWN))
                ui::OpenPopup("Attribute Menu");

            if (ui::BeginPopup("Attribute Menu"))
            {
                if (ui::MenuItem("Reset to default"))
                {
                    _undo.TrackValue(item, info.name_, value);
                    item->SetAttribute(info.name_, info.defaultValue_);
                    item->ApplyAttributes();
                    _undo.TrackValue(item, info.name_, info.defaultValue_);
                }

                if (style_variant != value)
                {
                    if (!style_variant.IsEmpty())
                    {
                        if (ui::MenuItem("Reset to style"))
                        {
                            _undo.TrackValue(item, info.name_, value);
                            item->SetAttribute(info.name_, style_variant);
                            item->ApplyAttributes();
                            _undo.TrackValue(item, info.name_, style_variant);
                        }
                    }

                    if (style_xml.NotNull())
                    {
                        if (ui::MenuItem("Save to style"))
                        {
                            if (style_attribute.IsNull())
                            {
                                style_attribute = style_xml.CreateChild("attribute");
                                style_attribute.SetAttribute("name", info.name_);
                            }
                            style_attribute.SetVariant(value);
                        }
                    }
                }

                if (style_attribute.NotNull())
                {
                    if (ui::MenuItem("Remove from style"))
                        style_attribute.GetParent().RemoveChild(style_attribute);
                }

                ImGui::EndPopup();
            }
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

    String GetBaseName(const String& full_path)
    {
        auto parts = full_path.Split('/');
        return parts.At(parts.Size() - 1);
    }

    void UpdateWindowTitle()
    {
        String window_name = "UrhoUIEditor";
        if (!_current_file_path.Empty())
            window_name += " - " + GetBaseName(_current_file_path);
        if (!_current_style_file_path.Empty())
            window_name += " - " + GetBaseName(_current_style_file_path);
        context_->GetGraphics()->SetWindowTitle(window_name);
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

    void GetStyleData(const AttributeInfo& info, XMLElement& style, XMLElement& attribute, Variant& value)
    {
        static XPathQuery _xp_attribute("attribute[@name=$name]", "name:String");
        static XPathQuery _xp_style("/elements/element[@type=$type]", "type:String");

        _xp_attribute.SetVariable("name", info.name_);
        style = _selected->GetStyleElement();
        value = Variant();

        if (style.NotNull())
        {
            attribute = style.SelectSinglePrepared(_xp_attribute);
            if (attribute.IsNull())
            {
                auto style_name = _selected->GetAppliedStyle();
                while (!style_name.Empty())
                {
                    _xp_style.SetVariable("type", style_name);
                    style = _style_file->GetRoot().SelectSinglePrepared(_xp_style);
                    if (style.NotNull())
                        style_name = style.GetAttribute("Style");
                    else
                        return;
                }
                attribute = style.SelectSinglePrepared(_xp_attribute);
            }
        }

        if (!attribute.IsNull())
        {
            value = attribute.GetVariantValue(info.enumNames_ ? VAR_STRING : info.type_);
            if (info.enumNames_)
            {
                for (auto i = 0; info.enumNames_[i]; i++)
                {
                    if (value.GetString() == info.enumNames_[i])
                    {
                        value = i;
                        break;
                    }
                }
            }
        }
    }
};

ATOMIC_DEFINE_APPLICATION_MAIN(UIEditorApplication);
