#pragma once

#include "type_writers.h"
#include "helpers.h"

namespace cppwin32
{
    struct finish_with
    {
        writer& w;
        void (*finisher)(writer&);

        finish_with(writer& w, void (*finisher)(writer&)) : w(w), finisher(finisher) {}
        finish_with(finish_with const&) = delete;
        void operator=(finish_with const&) = delete;

        ~finish_with() { finisher(w); }
    };

    void write_include_guard(writer& w)
    {
        auto format = R"(#pragma once
)";

        w.write(format);
    }

    void write_close_namespace(writer& w)
    {
        auto format = R"(}
)";

        w.write(format);
    }

    [[nodiscard]] static finish_with wrap_impl_namespace(writer& w)
    {
        auto format = R"(namespace win32::_impl_
{
)";

        w.write(format);

        return { w, write_close_namespace };
    }

    [[nodiscard]] finish_with wrap_type_namespace(writer& w, std::string_view const& ns)
    {
        // TODO: Move into forwards
        auto format = R"(WIN32_EXPORT namespace win32::@
{
)";

        w.write(format, ns);

        return { w, write_close_namespace };
    }

    void write_enum_field(writer& w, Field const& field)
    {
        auto format = R"(        % = %,
)";

        if (auto constant = field.Constant())
        {
            w.write(format, field.Name(), *constant);
        }
    }

    void write_enum(writer& w, TypeDef const& type)
    {
        auto format = R"(    enum class % : %
    {
%    };
)";

        auto fields = type.FieldList();
        w.write(format, type.TypeName(), fields.first.Signature().Type(), bind_each<write_enum_field>(fields));
    }

    void write_forward(writer& w, TypeDef const& type)
    {
        auto format = R"(    struct %;
)";

        w.write(format, type.TypeName());
    }

    struct struct_field
    {
        std::string_view name;
        std::string type;
        std::optional<int32_t> array_count;
    };

    void write_struct_field(writer& w, struct_field const& field)
    {
        if (field.array_count)
        {
            w.write("        @ %[%];\n",
                field.type, field.name, field.array_count.value());
        }
        else
        {
            w.write("        @ %;\n",
                field.type, field.name);
        }
    }

    TypeDef get_nested_type(TypeSig const& type)
    {
        auto index = std::get_if<coded_index<TypeDefOrRef>>(&type.Type());
        TypeDef result{};
        if (index && index->type() == TypeDefOrRef::TypeDef && index->TypeDef().EnclosingType())
        {
            result = index->TypeDef();
        }
        return result;
    }

    void write_struct(writer& w, TypeDef const& type)
    {
        auto format = R"(    struct %
    {
%    };
)";
        auto const name = type.TypeName();
        if (name == "BOOT_AREA_INFO")
        {
            std::string temp = name.data();
        }
        struct complex_struct
        {
            complex_struct(writer& w, TypeDef const& type)
                : type(type)
            {
                fields.reserve(size(type.FieldList()));
                for (auto&& field : type.FieldList())
                {
                    auto const name = field.Name();
                    auto field_type = field.Signature().Type();
                    std::optional<int32_t> array_count;
                    
                    if (auto nested_type = get_nested_type(field_type))
                    {
                        auto buffer_attribute = get_attribute(field, "System.Runtime.CompilerServices", "FixedBufferAttribute");
                        if (buffer_attribute)
                        {
                            auto const& sig = buffer_attribute.Value();
                            if (sig.FixedArgs().size() != 2)
                            {
                                throw std::invalid_argument("FixedBufferAttribute should have 2 args");
                            }
                            array_count = std::get<int32_t>(std::get<ElemSig>(sig.FixedArgs()[1].value).value);
                            auto nested_type = std::get<coded_index<TypeDefOrRef>>(field_type.Type());
                            field_type = nested_type.TypeDef().FieldList().first.Signature().Type();
                            continue;
                        }
                        else if (nested_type.TypeName().find("__FixedBuffer") != std::string_view::npos)
                        {
                            array_count = static_cast<int32_t>(size(nested_type.FieldList()));
                            field_type = nested_type.FieldList().first.Signature().Type();
                            continue;
                        }
                        else if (nested_type.Flags().Layout() == TypeLayout::ExplicitLayout && nested_type.TypeName().find("_e__Union") != std::string_view::npos)
                        {
                            // TODO: unions
                            continue;
                        }
                        else if (nested_type.TypeName().find("_e__Struct") != std::string_view::npos)
                        {
                            // TODO: unions
                            continue;
                        }
                        continue;
                    }
                    
                    fields.push_back({ name, w.write_temp("%", field_type), array_count });
                }
            }

            TypeDef type;
            std::vector<struct_field> fields;
        };

        complex_struct s{ w, type };

        w.write(format, type.TypeName(), bind_each<write_struct_field>(s.fields));
    }

    struct dependency_sorter
    {
        struct node
        {
            std::vector<TypeDef> edges;
            bool temporary{};
            bool permanent{};

            // Number of edges on an individual node should be small, so linear search is fine.
            void add_edge(TypeDef const& edge)
            {
                if (std::find(edges.begin(), edges.end(), edge) == edges.end())
                {
                    edges.push_back(edge);
                }
            }
        };

        std::map<TypeDef, node> dependency_map;
        using value_type = std::map<TypeDef, node>::value_type;

        void add(TypeDef const& type)
        {
#ifdef _DEBUG
            auto type_name = type.TypeName();
#endif
            auto [it, inserted] = dependency_map.insert({ type, {} });
            if (!inserted) return;
            for (auto&& field : type.FieldList())
            {
#ifdef _DEBUG
                auto field_name = field.Name();
#endif
                auto const signature = field.Signature();
                if (signature.Type().ptr_count() == 0)
                {
                    if (auto const field_type = std::get_if<coded_index<TypeDefOrRef>>(&signature.Type().Type()))
                    {
                        if (field_type->type() == TypeDefOrRef::TypeDef)
                        {
                            auto field_type_def = field_type->TypeDef();
                            if (get_category(field_type_def) != category::enum_type)
                            {
                                it->second.add_edge(field_type_def);
                                add(field_type_def);
                            }
                        }
                    }
                }
            }
        }

        void visit(value_type& v, std::vector<TypeDef>& sorted)
        {
#ifdef _DEBUG
            auto type_name = v.first.TypeName();
#endif

            if (v.second.permanent) return;
            if (v.second.temporary) throw std::invalid_argument("Cyclic dependency graph encountered");

            v.second.temporary = true;
            for (auto&& edge : v.second.edges)
            {
                auto it = dependency_map.find(edge);
                XLANG_ASSERT(it != dependency_map.end());
                visit(*it, sorted);
            }
            v.second.temporary = false;
            v.second.permanent = true;
            if (!v.first.EnclosingType())
            {
                sorted.push_back(v.first);
            }
        }

        std::vector<TypeDef> sort()
        {
            std::vector<TypeDef> result;
            auto eligible = [](value_type const& v) { return !v.second.permanent; };
            for (auto it = std::find_if(dependency_map.begin(), dependency_map.end(), eligible)
                ; it != dependency_map.end()
                ; it = std::find_if(dependency_map.begin(), dependency_map.end(), eligible))
            {
                visit(*it, result);
            }
            return result;
        }
    };

    void write_structs(writer& w, std::vector<TypeDef> const& structs)
    {
        dependency_sorter ds;
        for (auto&& type : structs)
        {
            ds.add(type);
        }
        
        auto sorted_structs = ds.sort();
        w.write_each<write_struct>(sorted_structs);
    }

    void write_abi_params(writer& w, method_signature const& method_signature)
    {
        separator s{ w };
        for (auto&& [param, param_signature] : method_signature.params())
        {
            s();
            std::string type;
            if (param.Flags().HasFieldMarshal())
            {
                auto fieldMarshal = param.FieldMarshal();
                switch (fieldMarshal.Signature().type)
                {
                case NativeType::Lpstr:
                    if (param.Flags().In())
                    {
                        type = "const char*";
                    }
                    else
                    {
                        type = "char*";
                    }
                    break;

                case NativeType::Lpwstr:
                    if (param.Flags().In())
                    {
                        type = "const wchar_t*";
                    }
                    else
                    {
                        type = "wchar_t*";
                    }
                    break;

                default:
                    type = w.write_temp("%", param_signature->Type());
                    break;
                }
            }
            else
            {
                type = w.write_temp("%", param_signature->Type());
            }
            w.write("% %", type, param.Name());
        }
    }

    void write_abi_return(writer& w, RetTypeSig const& sig)
    {
        if (sig)
        {
            w.write(sig.Type());
        }
        else
        {
            w.write("void");
        }
    }

    int get_param_size(ParamSig const& param)
    {
        if (auto e = std::get_if<ElementType>(&param.Type().Type()))
        {
            if (param.Type().ptr_count() == 0)
            {
                switch (*e)
                {
                case ElementType::U8:
                case ElementType::I8:
                case ElementType::R8:
                    return 8;

                default:
                    return 4;
                }
            }
            else
            {
                return 4;
            }
        }
        else
        {
            return 4;
        }
    }

    void write_abi_link(writer& w, method_signature const& method_signature)
    {
        int count = 0;
        for (auto&& [param, param_signature] : method_signature.params())
        {
            count += get_param_size(*param_signature);
        }
        w.write("%, %", method_signature.method().Name(), count);
    }

    void write_class_abi(writer& w, TypeDef const& type)
    {
        w.write(R"(extern "C"
{
)");
        auto const format = R"xyz(    % __stdcall WIN32_IMPL_%(%) noexcept;
)xyz";
        auto guard = w.push_full_namespace(true);

        for (auto&& method : type.MethodList())
        {
            if (method.Flags().Access() == MemberAccess::Public)
            {
                method_signature signature{ method };
                w.write(format, bind<write_abi_return>(signature.return_signature()), method.Name(), bind<write_abi_params>(signature));
            }
        }
        w.write(R"(}
)");

        for (auto&& method : type.MethodList())
        {
            if (method.Flags().Access() == MemberAccess::Public)
            {
                method_signature signature{ method };
                w.write("WIN32_IMPL_LINK(%)\n", bind<write_abi_link>(signature));
            }
        }
        w.write("\n");
    }
    
    void write_method_params(writer& w, method_signature const& method_signature)
    {
        separator s{ w };
        for (auto&& [param, param_signature] : method_signature.params())
        {
            s();
            std::string type;
            if (param.Flags().HasFieldMarshal())
            {
                auto fieldMarshal = param.FieldMarshal();
                switch (fieldMarshal.Signature().type)
                {
                case NativeType::Lpstr:
                    if (param.Flags().In())
                    {
                        type = "const char*";
                    }
                    else
                    {
                        type = "char*";
                    }
                    break;

                case NativeType::Lpwstr:
                    if (param.Flags().In())
                    {
                        type = "const wchar_t*";
                    }
                    else
                    {
                        type = "wchar_t*";
                    }
                    break;

                default:
                    type = w.write_temp("%", param_signature->Type());
                    break;
                }
            }
            else
            {
                type = w.write_temp("%", param_signature->Type());
            }
            w.write("% %", type, param.Name());
        }
    }

    void write_method_args(writer& w, method_signature const& method_signature)
    {
        separator s{ w };
        for (auto&& [param, param_signature] : method_signature.params())
        {
            s();
            w.write(param.Name());
        }
    }

    void write_method_return(writer& w, method_signature const& method_signature)
    {
        auto const& ret = method_signature.return_signature();
        if (ret)
        {
            w.write(ret.Type());
        }
        else
        {
            w.write("void");
        }
    }

    void write_class_method(writer& w, method_signature const& method_signature)
    {
        auto const format = R"xyz(        %% %(%)
        {
            return WIN32_IMPL_%(%);
        }
)xyz";
        std::string_view modifier;
        if (method_signature.method().Flags().Static())
        {
            modifier = "static ";
        }
        w.write(format, modifier, bind<write_method_return>(method_signature), method_signature.method().Name(), bind<write_method_params>(method_signature),
            method_signature.method().Name(), bind<write_method_args>(method_signature));
    }

    void write_class(writer& w, TypeDef const& type)
    {
        {
            auto const format = R"(    struct %
    {
)";
            w.write(format, type.TypeName());
        }
        for (auto&& method : type.MethodList())
        {
            if (method.Flags().Access() == MemberAccess::Public)
            {
                method_signature signature{ method };
                write_class_method(w, signature);
            }
        }
        w.write(R"(
    };
)");
    }

    void write_delegate_params(writer& w, method_signature const& method_signature)
    {
        separator s{ w };
        for (auto&& [param, param_signature] : method_signature.params())
        {
            s();
            std::string type;
            if (param.Flags().HasFieldMarshal())
            {
                auto fieldMarshal = param.FieldMarshal();
                switch (fieldMarshal.Signature().type)
                {
                case NativeType::Lpstr:
                    if (param.Flags().In())
                    {
                        type = "const char*";
                    }
                    else
                    {
                        type = "char*";
                    }
                    break;

                case NativeType::Lpwstr:
                    if (param.Flags().In())
                    {
                        type = "const wchar_t*";
                    }
                    else
                    {
                        type = "wchar_t*";
                    }
                    break;

                default:
                    type = w.write_temp("%", param_signature->Type());
                    break;
                }
            }
            else
            {
                type = w.write_temp("%", param_signature->Type());
            }
            w.write("%", type);
        }
    }

    void write_delegate(writer& w, TypeDef const& type)
    {
        auto const format = R"xyz(    using % = std::add_pointer_t<% __stdcall(%)>;
)xyz";
        MethodDef invoke;
        for (auto&& method : type.MethodList())
        {
            if (method.Name() == "Invoke")
            {
                invoke = method;
                break;
            }
        }
        method_signature method_signature{ invoke };

        w.write(format, type.TypeName(), bind<write_method_return>(method_signature), bind<write_delegate_params>(method_signature));
    }

    void write_enum_operators(writer& w, TypeDef const& type)
    {
        if (!get_attribute(type, "System", "FlagsAttribute"))
        {
            return;
        }

        auto name = type.TypeName();

        auto format = R"(    constexpr auto operator|(% const left, % const right) noexcept
    {
        return static_cast<%>(_impl_::to_underlying_type(left) | _impl_::to_underlying_type(right));
    }
    constexpr auto operator|=(%& left, % const right) noexcept
    {
        left = left | right;
        return left;
    }
    constexpr auto operator&(% const left, % const right) noexcept
    {
        return static_cast<%>(_impl_::to_underlying_type(left) & _impl_::to_underlying_type(right));
    }
    constexpr auto operator&=(%& left, % const right) noexcept
    {
        left = left & right;
        return left;
    }
    constexpr auto operator~(% const value) noexcept
    {
        return static_cast<%>(~_impl_::to_underlying_type(value));
    }
    constexpr auto operator^^(% const left, % const right) noexcept
    {
        return static_cast<%>(_impl_::to_underlying_type(left) ^^ _impl_::to_underlying_type(right));
    }
    constexpr auto operator^^=(%& left, % const right) noexcept
    {
        left = left ^^ right;
        return left;
    }
)";
        w.write(format, name, name, name, name, name, name, name, name, name, name, name, name, name, name, name, name, name);
    }

    struct guid
    {
        uint32_t Data1;
        uint16_t Data2;
        uint16_t Data3;
        uint8_t  Data4[8];
    };

    guid to_guid(std::string_view const& str)
    {
        if (str.size() < 36)
        {
            throw_invalid("Invalid GuidAttribute blob");
        }
        guid result;
        auto const data = str.data();
        std::from_chars(data,      data + 8,  result.Data1, 16);
        std::from_chars(data + 9,  data + 13, result.Data2, 16);
        std::from_chars(data + 14, data + 18, result.Data3, 16);
        std::from_chars(data + 19, data + 21, result.Data4[0], 16);
        std::from_chars(data + 21, data + 23, result.Data4[1], 16);
        std::from_chars(data + 24, data + 26, result.Data4[2], 16);
        std::from_chars(data + 26, data + 28, result.Data4[3], 16);
        std::from_chars(data + 28, data + 30, result.Data4[4], 16);
        std::from_chars(data + 30, data + 32, result.Data4[5], 16);
        std::from_chars(data + 32, data + 34, result.Data4[6], 16);
        std::from_chars(data + 34, data + 36, result.Data4[7], 16);
        return result;
    }

    void write_guid_value(writer& w, guid const& g)
    {
        w.write_printf("0x%08X,0x%04X,0x%04X,{ 0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X }",
            g.Data1,
            g.Data2,
            g.Data3,
            g.Data4[0],
            g.Data4[1],
            g.Data4[2],
            g.Data4[3],
            g.Data4[4],
            g.Data4[5],
            g.Data4[6],
            g.Data4[7]);
    }

    void write_guid(writer& w, TypeDef const& type)
    {
        if (type.TypeName() == "IUnknown")
        {
            return;
        }
        auto attribute = get_attribute(type, "System.Runtime.InteropServices", "GuidAttribute");
        if (!attribute)
        {
            throw_invalid("'System.Runtime.InteropServices.GuidAttribute' attribute for type '", type.TypeNamespace(), ".", type.TypeName(), "' not found");
        }

        auto const sig = attribute.Value();
        auto const guid_str = std::get<std::string_view>(std::get<ElemSig>(sig.FixedArgs()[0].value).value);
        auto const guid_value = to_guid(guid_str);

        auto format = R"(    template <> inline constexpr guid guid_v<%>{ % }; // %
)";

        w.write(format,
            type,
            bind<write_guid_value>(guid_value),
            guid_str);
    }

    bool should_write_interface(TypeDef const& type)
    {
        return type.TypeName() != "IUnknown" && size(type.MethodList()) >= 3;
    }
    
    std::pair<MethodDef, MethodDef> non_inherited_methods(TypeDef const& type)
    {
        auto method_list = type.MethodList();
        XLANG_ASSERT(size(method_list) >= 3);
        // Skip IUnknown methods
        method_list.first += 3;
        return method_list;
    }

    void write_interface_abi(writer& w, TypeDef const& type)
    {
        if (!should_write_interface(type))
        {
            return;
        }

        {
            auto const format = R"(    template <> struct abi<%>
    {
        struct __declspec(novtable) type : unknown_abi
        {
)";
            w.write(format, type);
        }

        auto const format = R"(            virtual % __stdcall %(%) noexcept = 0;
)";
        auto abi_guard = w.push_abi_types(true);
        
        for (auto&& method : non_inherited_methods(type))
        {
            method_signature signature{ method };
            w.write(format, bind<write_abi_return>(signature.return_signature()), method.Name(), bind<write_abi_params>(signature));
        }

        w.write(R"(        };
    };
)");
    }

    void write_consume_params(writer& w, method_signature const& signature)
    {
        separator s{ w };

        for (auto&& [param, param_signature] : signature.params())
        {
            s();

            w.write("% %", param_signature->Type(), param.Name());

            //if (param.Flags().In())
            //{
            //    XLANG_ASSERT(!param.Flags().Out());

            //    auto const param_type = std::get_if<ElementType>(&param_signature->Type().Type());

            //    if (param_type)
            //    {
            //        w.write("%", param_signature->Type());
            //    }
            //    else
            //    {
            //        w.write("% const&", param_signature->Type());
            //    }
            //}
            //else
            //{
            //    XLANG_ASSERT(!param.Flags().In());
            //    XLANG_ASSERT(param.Flags().Out());
            //    w.write("%&", param_signature->Type());
            //}

            //w.write(" %", param.Name());
        }
    }

    void write_consume_declaration(writer& w, MethodDef const& method)
    {
        method_signature const signature{ method };
        auto const name = method.Name();
        w.write("        WIN32_IMPL_AUTO(%) %(%) const;\n",
            signature.return_signature(),
            name,
            bind<write_consume_params>(signature));
    }

    void write_consume(writer& w, TypeDef const& type)
    {
        if (!should_write_interface(type))
        {
            return;
        }

        auto const impl_name = get_impl_name(type.TypeNamespace(), type.TypeName());

        auto const format = R"(    struct consume_%
    {
%    };
)";

        w.write(format,
            impl_name,
            bind_each<write_consume_declaration>(non_inherited_methods(type)));
    }

    void write_interface(writer& w, TypeDef const& type)
    {
        if (!should_write_interface(type))
        {
            return;
        }

        auto const type_name = type.TypeName();

        auto const format = R"(    struct __declspec(empty_bases) % :
        Microsoft::Windows::Sdk::IUnknown,
        _impl_::consume_%
    {
        %(std::nullptr_t = nullptr) noexcept {}
        %(void* ptr, take_ownership_from_abi_t) noexcept : Microsoft::Windows::Sdk::IUnknown(ptr, take_ownership_from_abi) {}
    };
)";
        w.write(format,
            type_name,
            get_impl_name(type.TypeNamespace(), type_name),
            type_name,
            type_name);
    }
}
