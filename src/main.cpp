#include "loader/prx_reader.hpp"
#include "runtime/execution.hpp"

#include <fmt/core.h>

#include <charconv>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

int main(int argc, char *argv[])
{
    try
    {
        if (argc != 2 && argc != 4)
        {
            throw std::invalid_argument("Usage: pspemu program.prx [--max-instructions count]");
        }
        psp::ExecutionOptions options;
        options.arguments = {argv[1]};
        if (argc == 4)
        {
            if (std::string_view(argv[2]) != "--max-instructions")
            {
                throw std::invalid_argument("Expected --max-instructions");
            }
            const std::string_view value(argv[3]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.max_instructions);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                throw std::invalid_argument("Invalid instruction budget");
            }
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Cannot open PRX file");
        }
        const psp::Payload payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto result = psp::execute_prx(psp::read_prx(payload), options);
        fmt::print("{}", result.output);
        return result.exit_code;
    }
    catch (const std::exception &error)
    {
        fmt::print(stderr, "{}\n", error.what());
        return 1;
    }
}
