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
#include <Atomic/Scene/Scene.h>
#include <Atomic/Input/Input.h>
#include <Atomic/Graphics/GraphicsEvents.h>

#include <UrhoUI.h>
#include <unordered_map>
#include <tinyfiledialogs.h>
#include "IconsFontAwesome.h"


using namespace std::placeholders;
using namespace Atomic;
using namespace Atomic::UrhoUI;
namespace ui=ImGui;


class UIEditorApplication : public Application
{
    ATOMIC_OBJECT(UIEditorApplication, Application);
public:
    SharedPtr<Scene> _scene;
    WeakPtr<UrhoUI::UI> _ui;
    WeakPtr<UIElement> _selected;
    HashMap<String, std::array<char, 0x1000>> _buffers;
    String _current_file_path;

    explicit UIEditorApplication(Context* ctx) : Application(ctx)
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
        auto zone = _scene->CreateComponent<Zone>();
        zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
        zone->SetFogColor(Color(0.1f, 0.1f, 0.1f));
        zone->SetFogStart(100.0f);
        zone->SetFogEnd(300.0f);
        GetSubsystem<Renderer>()->SetViewport(0, new Viewport(context_, _scene, _scene->CreateChild("Camera")->CreateComponent<Camera>()));

        // Events
        SubscribeToEvent(E_SYSTEMUIFRAME, std::bind(&UIEditorApplication::RenderSystemUI, this));
        SubscribeToEvent(E_DROPFILE, std::bind(&UIEditorApplication::OnFileDrop, this, _2));

        // Arguments
        if (GetArguments().Size() > 0)
            LoadFile(GetArguments().At(0));
    }

    void Stop() override
    {
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
                    const char* filters[] = {"*.json", "*.xml"};
                    auto filename = tinyfd_openFileDialog("Open file", ".", 2, filters, "UI files", 0);
                    if (filename)
                        LoadFile(filename);
                }

                if (ui::MenuItem(ICON_FA_FLOPPY_O " Save XML As") && _ui->GetRoot()->GetNumChildren() > 0)
                {
                    const char* filters[] = {"*.xml"};
                    if (auto path = tinyfd_saveFileDialog("Save XML", ".", 1, filters, "XML files"))
                    {
                        if (SaveFileXML(path))
                            _current_file_path = path;
                    }
                }

                ui::EndMenu();
            }
            if (ui::Button(ICON_FA_FLOPPY_O) && !_current_file_path.Empty())
                SaveFileXML(_current_file_path);
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
                _selected = clicked;
        }

        if (_selected)
        {
            if (input->GetKeyPress(KEY_DELETE) && _selected != _ui->GetRoot())
                _selected->Remove();

            if (ImGui::BeginPopupContextVoid("Element Context Menu", 2))
            {
                if (ImGui::BeginMenu("Add Child"))
                {
                    const char* ui_types[] = {
                        "BorderImage",
                        "Button",
                        "CheckBox",
                        "Cursor",
                        "DropDownList",
                        "LineEdit",
                        "ListView",
                        "Menu",
                        "ProgressBar",
                        "ScrollBar",
                        "ScrollView",
                        "Slider",
                        "Sprite",
                        "Text",
                        "ToolTip",
                        "UIElement",
                        "View3D",
                        "Window",
                        0
                    };
                    for (auto i = 0; ui_types[i] != 0; i++)
                    {
                        if (ui::MenuItem(ui_types[i]))
                        {
                            _selected = _selected->CreateChild(ui_types[i]);
                            _selected->SetStyleAuto();
                        }
                    }
                    ImGui::EndMenu();
                }

                if (_selected != _ui->GetRoot())
                {
                    if (ImGui::Selectable("Delete Element"))
                        _selected->Remove();
                }
                ImGui::EndPopup();
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

        SharedPtr<XMLFile> xml(new XMLFile(context_));
        if (xml->LoadFile(file_path))
        {
            auto resource_dir = GetResourcePath(file_path);
            if (!cache->GetResourceDirs().Contains(resource_dir))
                cache->AddResourceDir(resource_dir);

            if (xml->GetRoot().GetName() == "elements")
            {
                // This is a style.
                _ui->GetRoot()->SetDefaultStyle(xml);
                return true;
            }
            else if (xml->GetRoot().GetName() == "element")
            {
                auto child = _ui->GetRoot()->CreateChild<UIElement>();
                if (child->LoadXML(xml->GetRoot()))
                {
                    child->SetStyleAuto();
                    _current_file_path = file_path;
                    return true;
                }
                else
                    child->Remove();
            }
        }

        tinyfd_messageBox("Error", "Opening XML file failed", "ok", "error", 1);
        return false;
    }

    bool SaveFileXML(const String& file_path)
    {
        File saveFile(context_, file_path, FILE_WRITE);
        if (!_ui->GetRoot()->GetChild(0)->SaveXML(saveFile))
        {
            tinyfd_messageBox("Error", "Saving XML file failed", "ok", "error", 1);
            return false;
        }
        return true;
    }

    void RenderUITree(UIElement* element)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!element->IsInternal())
            flags |= ImGuiTreeNodeFlags_DefaultOpen;

        if (element == _selected)
            flags |= ImGuiTreeNodeFlags_Selected;

        auto& name = element->GetName();
        auto& type = element->GetTypeName();
        if (ui::TreeNodeEx(element, flags, "%s", name.Length() ? name.CString() : type.CString()))
        {
            auto input = context_->GetInput();
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
                _selected = element;

            for (auto child: element->GetChildren())
                RenderUITree(child);
            ui::TreePop();
        }
        if (ui::IsItemHovered())
            ui::SetTooltip("Type: %s\nInternal: %s", type.CString(), element->IsInternal() ? "true" : "false");
    }

    void RenderAttributes(Serializable* item)
    {
        ui::Columns(2);
        ui::PushID(item);
        const auto& attributes = *item->GetAttributes();
        for (const AttributeInfo& info: attributes)
        {
            if (info.accessor_.Null() || info.mode_ & AM_NOEDIT)
                continue;

            ui::PushID(info.name_.CString());

            ui::TextUnformatted(info.name_.CString());
            ui::NextColumn();

            if (ui::Button(ICON_FA_UNDO))
                info.accessor_->Set(item, info.defaultValue_);
            ui::SameLine();

            Variant value;
            info.accessor_->Get(item, value);
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
                    char buf[1024];
                    strcpy(buf, v.CString());
                    modified |= ui::InputText("", buf, sizeof(buf) - 1);
                    if (modified)
                        value = buf;
                    break;
                }
//            case VAR_BUFFER:
                case VAR_VOIDPTR:
                    ui::Text("%p", value.GetVoidPtr());
                    break;
                case VAR_RESOURCEREF:
                    ui::Text("%s", value.GetResourceRef().name_.CString());
                    break;
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
                    auto& buffer = _buffers[info.name_];
                    ui::PushID(index++);
                    if (ui::InputText("", &buffer.front(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        v.Push(&buffer.front());
                        buffer.front() = 0;
                        modified = true;
                    }
                    ui::PopID();

                    for (String& sv: v)
                    {
                        ui::PushID(index++);
                        if (ui::Button(ICON_FA_TRASH))
                        {
                            v.Remove(sv);
                            modified = true;
                        }
                        if (modified)
                        {
                            ui::PopID();
                            break;
                        }
                        ui::SameLine();

                        if (sv.Capacity() - 2 < sv.Length())
                            sv.Reserve(sv.Length() + 2);
                        modified |= ui::InputText("", const_cast<char*>(sv.CString()), sv.Capacity());
                        if (modified)
                            sv.Resize(strlen(sv.CString()));
                        ui::PopID();
                    }
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
            ui::PopID();
            ui::NextColumn();

            if (modified)
            {
                info.accessor_->Set(item, value);
                item->ApplyAttributes();
            }
        }
        ui::PopID();
        ui::Columns(1);
    }
};

ATOMIC_DEFINE_APPLICATION_MAIN(UIEditorApplication);
