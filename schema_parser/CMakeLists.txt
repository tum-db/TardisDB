set(SCHEMA_PARSER_SRCS
        generator.cpp
        generator.hpp
        main.cpp
        Parser.cpp
        Parser.hpp
        Schema.cpp
        Schema.hpp
        Types.hpp
        )

add_executable(schema_parser ${SCHEMA_PARSER_SRCS})
