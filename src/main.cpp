#include <city/JIT.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tree_sitter/api.h>
#include <unordered_map>

extern "C" TSLanguage const *tree_sitter_c();

struct CompilationContext
{
    std::string_view source;
    city::IRBuilder &builder;

    std::unordered_map<std::string, city::IRFunction *> functions;
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
            return ctx.builder.GetType<double>();
        }

        if (raw_value == "int")
        {
            return ctx.builder.GetType<int>();
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

    city::Value *ProcessBinaryExpression(CompilationContext &ctx, TSNode node)
    {
        city::Value *lhs = this->ProcessValue(ctx, ts_node_child_by_field_name(node, "left", 4));
        city::Value *rhs = this->ProcessValue(ctx, ts_node_child_by_field_name(node, "right", 5));
        auto op = this->GetNodeRawValue(ctx, ts_node_child_by_field_name(node, "operator", 8));

        if (op == "+")
        {
            auto addtmp = ctx.builder.InsertAddInst(lhs, rhs);
            return addtmp->GetReturnValue();
        }

        if (op == "-")
        {
            auto subtmp = ctx.builder.InsertSubInst(lhs, rhs);
            return subtmp->GetReturnValue();
        }

        throw std::runtime_error("unrecognized operator");
    }

    city::Value *ProcessValue(CompilationContext &ctx, TSNode node)
    {
        if (std::strcmp(ts_node_type(node), "number_literal") == 0)
        {
            return this->ProcessNumberLiteral(ctx, node);
        }

        if (std::strcmp(ts_node_type(node), "binary_expression") == 0)
        {
            return this->ProcessBinaryExpression(ctx, node);
        }

        throw std::runtime_error("unsupported value type");
    }

    void ProcessReturnStatement(CompilationContext &ctx, TSNode node)
    {
        if (ts_node_named_child_count(node) == 0)
        {
            ctx.builder.InsertRetInst();
        }
        else
        {
            city::Value *return_value = this->ProcessValue(ctx, ts_node_named_child(node, 0));
            ctx.builder.InsertRetInst(return_value);
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
        }
    }

    void ProcessFunctionDefinition(CompilationContext &ctx, TSNode node)
    {
        // Type
        auto return_type = this->ProcessPrimitiveType(ctx, ts_node_child_by_field_name(node, "type", 5));

        // Function name and parameters
        TSNode declarator_node = ts_node_child_by_field_name(node, "declarator", 10);
        TSNode name_node = ts_node_child_by_field_name(declarator_node, "declarator", 10);
        auto function_name = std::string(this->GetNodeRawValue(ctx, name_node));

        // TODO: parse parameters

        auto function = ctx.builder.CreateFunction(function_name, return_type);
        ctx.functions[function_name] = function;

        TSNode body = ts_node_child_by_field_name(node, "body", 4);
        this->ProcessCompoundStatement(ctx, body);
    }

    void ProcessTranslationUnit(CompilationContext &ctx, TSNode node)
    {
        for (int i = 0; i < ts_node_named_child_count(node); i++)
        {
            TSNode child = ts_node_named_child(node, i);
            if (std::strcmp(ts_node_type(child), "function_definition") == 0)
            {
                this->ProcessFunctionDefinition(ctx, child);
            }
        }
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
        std::cerr << "Usage: cityc \"path/to/src.c\"" << std::endl;
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
