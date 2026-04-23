cmake_minimum_required(VERSION 3.10)

project(tuning_server C)
set(CMAKE_C_STANDARD 99)

set(APP_SRCS ${CMAKE_SOURCE_DIR}/src/main.c
	${CMAKE_SOURCE_DIR}/src/awe_pkt_mngt.c
	${CMAKE_SOURCE_DIR}/src/file_ops.c
	${CMAKE_SOURCE_DIR}/src/tcp_app.c
	)

add_executable(${PROJECT_NAME} ${APP_SRCS})

target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/inc)

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
