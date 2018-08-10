include_directories(${LLVM_INCLUDE_DIRS} ..)
add_definitions(${LLVM_DEFINITIONS})

set(PHI_TEST_SRCS
        phi_test.cpp
        ../codegen/CodeGen.cpp
        ../codegen/CodeGen.hpp
        ../codegen/CodeGen.tcc
        ../codegen/PhiNode.hpp
        ../foundations/LegacyTypes.cpp
        ../foundations/LegacyTypes.hpp
        ../sql/SqlType.cpp
        ../sql/SqlType.hpp
        ../sql/SqlValues.cpp
        ../sql/SqlValues.hpp
        )

add_executable(phi_test ${PHI_TEST_SRCS})

target_link_libraries(phi_test ${LLVM_LIBS})
#target_link_libraries(phi_test LLVM)
