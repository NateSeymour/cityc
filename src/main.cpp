#include <city/JIT.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tree_sitter/api.h>
#include <unordered_map>

extern "C" TSLanguage const *tree_sitter_c();

class Scope
{
    std::vector<std::unordered_map<std::string, city::Value *>> variables_;

public:
    void PushLayer()
    {
        this->variables_.emplace_back();
    }

    void PopLayer()
    {
        this->variables_.pop_back();
    }

    [[nodiscard]] city::Value *Lookup(std::string const &name) const
    {
        for (int i = this->variables_.size() - 1; i >= 0; i--)
        {
            if (this->variables_[i].contains(name))
            {
                return this->variables_[i].at(name);
            }
        }

        return nullptr;
    }

    void Set(std::string const &name, city::Value *value)
    {
        this->variables_.back()[name] = value;
    }
};

struct CompilationContext
{
    std::string_view source;
    city::IRBuilder &builder;

    std::unordered_map<std::string, city::IRFunction *> functions;
    Scope scope;
};

class Compiler
{
    city::JIT jit_;

    void PrintNodeTree(TSNode node, int depth = 0)
    {
        std::cout << std::string(depth, '\t') << ts_node_type(node) << std::endl;
        for (int i = 0; i < ts_node_named_child_count(node); i++)
        {
            this->PrintNodeTree(ts_node_named_child(node, i), depth + 1);
        }
    }

    std::string_view GetNodeRawValue(CompilationContext &ctx, TSNode node)
    {
        auto begin = ts_node_start_byte(node);
        auto end = ts_node_end_byte(node);

        return ctx.source.substr(begin, end - begin);
    }

    city::Type ProcessPrimitiveType(CompilationContext &ctx, TSNode node)
    {
        auto raw_value = this->GetNodeRawValue(ctx, node);

        if (raw_value == "double")
        {
            return city::Type::Get<double>();
        }

        if (raw_value == "int")
        {
            return city::Type::Get<int>();
        }

        if (raw_value == "void")
        {
            return city::Type::Get<void>();
        }

        throw std::runtime_error("unknown type name");
    }

    city::Value *ProcessNumberLiteral(CompilationContext &ctx, TSNode node, std::optional<city::Type> hint = std::nullopt)
    {
        auto raw_value = this->GetNodeRawValue(ctx, node);

        if (raw_value.contains('.'))
        {
            return ctx.builder.CreateConstant(std::stod(std::string(raw_value)));
        }
        else
        {
            return ctx.builder.CreateConstant(std::stoi(std::string(raw_value)));
        }
    }

    city::Value *ProcessVariable(CompilationContext &ctx, TSNode node)
    {
        auto raw_value = this->GetNodeRawValue(ctx, node);

        auto value = ctx.scope.Lookup(std::string(raw_value));

        if (!value)
        {
            throw std::runtime_error("undeclared identifier");
        }

        return value;
    }


    city::Value *ProcessBinaryExpression(CompilationContext &ctx, TSNode node)
    {
        city::Value *lhs = this->ProcessExpression(ctx, ts_node_child_by_field_name(node, "left", 4));
        city::Value *rhs = this->ProcessExpression(ctx, ts_node_child_by_field_name(node, "right", 5));
        auto op = this->GetNodeRawValue(ctx, ts_node_child_by_field_name(node, "operator", 8));

        if (op == "+")
        {
            return ctx.builder.InsertAddInst(lhs, rhs);
        }

        if (op == "-")
        {
            return ctx.builder.InsertSubInst(lhs, rhs);
        }

        throw std::runtime_error("unrecognized operator");
    }

    city::Value *ProcessCallExpression(CompilationContext &ctx, TSNode node)
    {
        // Get function name
        TSNode function_name_node = ts_node_child_by_field_name(node, "function", 8);
        auto function_name = std::string(this->GetNodeRawValue(ctx, function_name_node));

        // Get arguments
        TSNode argument_list_node = ts_node_child_by_field_name(node, "arguments", 9);
        std::vector<city::Value *> args;
        if(!ts_node_is_null(argument_list_node))
        {
            for(int i = 0; i < ts_node_named_child_count(argument_list_node); i++)
            {
                args.push_back(this->ProcessExpression(ctx, ts_node_named_child(argument_list_node, i)));
            }
        }

        return ctx.builder.InsertCallInst(ctx.functions[function_name], args);
    }

    city::Value *ProcessExpression(CompilationContext &ctx, TSNode node)
    {
        if (std::strcmp(ts_node_type(node), "number_literal") == 0)
        {
            return this->ProcessNumberLiteral(ctx, node);
        }

        if (std::strcmp(ts_node_type(node), "identifier") == 0)
        {
            return this->ProcessVariable(ctx, node);
        }

        if (std::strcmp(ts_node_type(node), "binary_expression") == 0)
        {
            return this->ProcessBinaryExpression(ctx, node);
        }

        if (std::strcmp(ts_node_type(node), "call_expression") == 0)
        {
            return this->ProcessCallExpression(ctx, node);
        }

        throw std::runtime_error("unsupported expression type");
    }

    void ProcessReturnStatement(CompilationContext &ctx, TSNode node)
    {
        if (ts_node_named_child_count(node) == 0)
        {
            ctx.builder.InsertRetInst();
        }
        else
        {
            city::Value *return_value = this->ProcessExpression(ctx, ts_node_named_child(node, 0));
            ctx.builder.InsertRetInst(return_value);
        }
    }

    void ProcessDeclaration(CompilationContext &ctx, TSNode node)
    {
        auto type = this->ProcessPrimitiveType(ctx, ts_node_child_by_field_name(node, "type", 4));

        TSNode declarator_node = ts_node_child_by_field_name(node, "declarator", 10);
        if (std::strcmp(ts_node_type(declarator_node), "identifier") == 0)
        {
            // TODO: declaration without value
        }
        else if (std::strcmp(ts_node_type(declarator_node), "init_declarator") == 0)
        {
            TSNode identifier_node = ts_node_child_by_field_name(declarator_node, "declarator", 10);
            auto variable_name = std::string(this->GetNodeRawValue(ctx, identifier_node));

            auto value = this->ProcessExpression(ctx, ts_node_child_by_field_name(declarator_node, "value", 5));

            ctx.scope.Set(variable_name, value);
        }
    }

    void ProcessCompoundStatement(CompilationContext &ctx, TSNode node)
    {
        for (int i = 0; i < ts_node_named_child_count(node); i++)
        {
            TSNode child = ts_node_named_child(node, i);
            if (std::strcmp(ts_node_type(child), "return_statement") == 0)
            {
                this->ProcessReturnStatement(ctx, child);
            }
            else if (std::strcmp(ts_node_type(child), "expression_statement") == 0)
            {
                if (ts_node_child_count(child) == 0)
                    continue;

                this->ProcessExpression(ctx, ts_node_child(child, 0));
            }
            else if (std::strcmp(ts_node_type(child), "declaration") == 0)
            {
                this->ProcessDeclaration(ctx, child);
            }
        }
    }

    void ProcessFunctionDefinition(CompilationContext &ctx, TSNode node)
    {
        ctx.scope.PushLayer();

        // Type
        auto return_type = this->ProcessPrimitiveType(ctx, ts_node_child_by_field_name(node, "type", 4));

        // Function name and parameters
        TSNode declarator_node = ts_node_child_by_field_name(node, "declarator", 10);
        TSNode name_node = ts_node_child_by_field_name(declarator_node, "declarator", 10);
        auto function_name = std::string(this->GetNodeRawValue(ctx, name_node));

        std::vector<std::string> arg_names;
        std::vector<city::Type> arg_types;
        TSNode parameter_list_node = ts_node_child_by_field_name(declarator_node, "parameters", 10);
        if (!ts_node_is_null(parameter_list_node))
        {
            for (int i = 0; i < ts_node_named_child_count(parameter_list_node); i++)
            {
                TSNode parameter_node = ts_node_named_child(parameter_list_node, i);
                TSNode parameter_name_node = ts_node_child_by_field_name(parameter_node, "declarator", 10);
                TSNode parameter_type_node = ts_node_child_by_field_name(parameter_node, "type", 4);

                arg_names.emplace_back(this->GetNodeRawValue(ctx, parameter_name_node));
                arg_types.push_back(this->ProcessPrimitiveType(ctx, parameter_type_node));
            }
        }

        // Create function
        auto function = ctx.builder.CreateFunction(function_name, return_type, arg_types);
        ctx.functions[function_name] = function;

        // Push parameter values into scope
        auto &args = function->GetArgs();
        for (int i = 0; i < arg_names.size(); i++)
        {
            ctx.scope.Set(arg_names[i], args[i]);
        }

        // Parse function body
        TSNode body = ts_node_child_by_field_name(node, "body", 4);
        this->ProcessCompoundStatement(ctx, body);

        ctx.scope.PopLayer();
    }

    void ProcessTranslationUnit(CompilationContext &ctx, TSNode node)
    {
        ctx.scope.PushLayer();

        for (int i = 0; i < ts_node_named_child_count(node); i++)
        {
            TSNode child = ts_node_named_child(node, i);
            if (std::strcmp(ts_node_type(child), "function_definition") == 0)
            {
                this->ProcessFunctionDefinition(ctx, child);
            }
        }

        ctx.scope.PopLayer();
    }

public:
    void InsertCSource(std::string name, std::string_view const text)
    {
        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_c());

        TSTree *source_tree = ts_parser_parse_string(parser, nullptr, text.data(), text.size());
        TSNode root = ts_tree_root_node(source_tree);

        this->PrintNodeTree(root);

        city::IRModule module{std::move(name)};
        auto builder = module.CreateBuilder();

        CompilationContext ctx{
                .source = text,
                .builder = builder,
        };
        this->ProcessTranslationUnit(ctx, root);

        ts_parser_delete(parser);
        this->jit_.InsertIRModule(std::move(module));
    }

    city::Assembly Compile()
    {
        return this->jit_.CompileAndLink();
    }
};

int main(int argc, char *argv[])
{
    // Open source file
    if (argc < 2)
    {
        std::cerr << "Usage: urban \"path/to/src1.c\" \"path/to/src2.c\"" << std::endl;
        return 1;
    }

    Compiler compiler;
    for (int i = 1; i < argc; i++)
    {
        std::filesystem::path path{argv[i]};
        std::ifstream file{path.c_str()};

        std::ostringstream stream;
        stream << file.rdbuf();
        std::string text = stream.str();

        std::cout << "Compiling module '" << path.filename() << "' from " << path << ":" << std::endl;
        std::cout << text << std::endl;

        compiler.InsertCSource(path.filename().string(), text);
    }

    auto assembly = compiler.Compile();
    auto entry = assembly["__entry"].ToPointer<int()>();

    int retval = entry();
    std::cout << "Program returned: " << retval << std::endl;

    return 0;
}
