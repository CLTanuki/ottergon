project(session)

set( ${PROJECT_NAME}_HEADERS
        session.hpp
)

set(${PROJECT_NAME}_SOURCES
        session.cpp

)

add_library(rocketjoe_${PROJECT_NAME}
        ${${PROJECT_NAME}_HEADERS}
        ${${PROJECT_NAME}_SOURCES}
)



add_library(rocketjoe::${PROJECT_NAME} ALIAS rocketjoe_${PROJECT_NAME})

set_property(TARGET rocketjoe_${PROJECT_NAME} PROPERTY EXPORT_NAME ${PROJECT_NAME})

target_include_directories(rocketjoe_${PROJECT_NAME} PUBLIC)

if (DEV_MODE)
    #add_subdirectory(tests)
endif()
