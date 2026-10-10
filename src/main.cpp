#include "frontend/window.hpp"
#include "loader/prx_reader.hpp"
#include "runtime/execution.hpp"

#include <fmt/core.h>

#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

int main(int argc, char *argv[])
{
    try
    {
        constexpr auto usage = "Usage: pspemu program.prx [--window] [--max-instructions count]";
        if (argc < 2)
        {
            throw std::invalid_argument(usage);
        }
        psp::ExecutionOptions options;
        // SDK startup code corrupts its stack on a leading '/' without a PSP drive prefix,
        // so the guest sees only the file name, independent of the host directory.
        options.arguments = {std::filesystem::path(argv[1]).filename().string()};
        bool window = false;
        bool budget_supplied = false;
        for (int index = 2; index < argc; ++index)
        {
            const std::string_view option(argv[index]);
            if (option == "--window" && !window)
            {
                window = true;
            }
            else if (option == "--max-instructions" && !budget_supplied && index + 1 < argc)
            {
                budget_supplied = true;
                const std::string_view value(argv[++index]);
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), options.max_instructions);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                {
                    throw std::invalid_argument("Invalid instruction budget");
                }
            }
            else
            {
                throw std::invalid_argument(usage);
            }
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Cannot open PRX file");
        }
        const psp::Payload payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto prx = psp::read_prx(payload);
        psp::Execution execution(prx, options);
        int exit_code = 0;
        try
        {
            if (window)
            {
                psp::frontend::Window display;
                exit_code = psp::frontend::run_windowed(execution, display);
            }
            else
            {
                execution.run_to_completion();
                exit_code = execution.result().exit_code;
            }
        }
        catch (const std::exception &)
        {
            // Output captured before a fault often explains it.
            fmt::print("{}", execution.output());
            throw;
        }
        fmt::print("{}", execution.output());
        return exit_code;
    }
    catch (const std::exception &error)
    {
        fmt::print(stderr, "{}\n", error.what());
        return 1;
    }
}
