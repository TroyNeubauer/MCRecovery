
workspace "MCRecovery"
	architecture "x64"
	startproject "MCRecovery"

	configurations
	{
		"Release",
	}

include "vendor/TUtil/TUtil_project.lua"

	project("MCRecovery")
		kind "ConsoleApp"

		files
		{
			"src/*",
		}

		includedirs
		{
			"src/",
		}

		links
		{
			"pthread",
		}

		TUtilDependencies()

