
macro(symlink source destination)
    if (NOT EXISTS ${destination})
        get_filename_component(DESTINATION_DIR "${destination}" DIRECTORY)
        file(MAKE_DIRECTORY ${DESTINATION_DIR})
        execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${source} ${destination})
    endif ()
endmacro()

file(GLOB_RECURSE SOURCE_FILES *.cpp *.h *.hpp)
add_executable(UIEditor ${SOURCE_FILES})
target_link_libraries(UIEditor Atomic UrhoUI tinyfiledialogs)

symlink (${CMAKE_SOURCE_DIR}/dep/AtomicGameEngine/Resources/CoreData ${CMAKE_BINARY_DIR}/bin/CoreData)
symlink (${CMAKE_SOURCE_DIR}/bin/UIEditorData ${CMAKE_BINARY_DIR}/bin/UIEditorData)
