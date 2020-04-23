cmake_minimum_required(VERSION 3.0)
project("MCRecovery")

add_custom_target(MCRecovery COMMAND make -j8
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		)

