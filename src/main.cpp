#include <city/JIT.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tree_sitter/api.h>

extern "C" TSLanguage const *tree_sitter_c();

struct CompilationContext
{
    city::IRBuilder &builder;
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

    city::Type ProcessPrimitiveType(CompilationContext &ctx, TSNode node)
    {
        if (std::strcmp(ts_node_string(node), "double") == 0)
        {
            return ctx.builder.GetType<double>();
        }

        if (std::strcmp(ts_node_string(node), "int") == 0)
        {
            return ctx.builder.GetType<int>();
        }

        throw std::runtime_error("unknown type name");
    }

    void ProcessFunctionBody(CompilationContext &ctx, TSNode node) {}

    void ProcessFunctionDefinition(CompilationContext &ctx, TSNode node)
    {
        auto return_type = this->ProcessPrimitiveType(ctx, ts_node_child_by_field_name(node, "type", 5));
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

    // Compile and run
    /*
    auto assembly = compiler.Compile();
    auto entry = assembly["__entry"].ToPointer<double()>();
    entry();
    */

    return 0;
}
