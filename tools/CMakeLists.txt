if(YETTY_ENABLE_TOOL_GPU_INFO)
    add_subdirectory(gpu-info)
endif()

if(YETTY_ENABLE_TOOL_CARD_RUNNER)
    add_subdirectory(card-runner)
endif()

if(YETTY_ENABLE_TOOL_YDRAW_GENERATOR)
    add_subdirectory(ydraw-generator)
endif()

if(YETTY_ENABLE_TOOL_YPAINT_BENCH)
    add_subdirectory(ypaint-bench)
endif()

if(YETTY_ENABLE_TOOL_YCAT)
    add_subdirectory(ycat)
endif()

if(YETTY_ENABLE_TOOL_YDRAW_MAZE)
    add_subdirectory(ydraw-maze)
endif()

if(YETTY_ENABLE_TOOL_YDRAW_ZOO)
    add_subdirectory(ydraw-zoo)
endif()

if(YETTY_ENABLE_TOOL_YMUX)
    add_subdirectory(ymux)
endif()

if(YETTY_ENABLE_TOOL_YFLAME)
    add_subdirectory(yflame)
endif()

if(YETTY_ENABLE_TOOL_PDF2YDRAW)
    add_subdirectory(pdf2ydraw)
endif()

if(YETTY_ENABLE_TOOL_HTML2YDRAW)
    add_subdirectory(html2ydraw)
endif()

if(YETTY_ENABLE_TOOL_YHTML_MACHINE)
    add_subdirectory(yhtml-machine)
endif()

if(YETTY_ENABLE_TOOL_YBROWSER)
    add_subdirectory(ybrowser)
endif()

if(YETTY_ENABLE_TOOL_VNC_RECORDER AND NOT WIN32)
    add_subdirectory(vnc-recorder)
endif()

if(YETTY_ENABLE_TOOL_YDOC)
    add_subdirectory(ydoc)
endif()

if(YETTY_ENABLE_TOOL_YSPREADSHEET)
    add_subdirectory(yspreadsheet)
endif()

if(YETTY_ENABLE_TOOL_YSLIDES)
    add_subdirectory(yslides)
endif()

if(YETTY_ENABLE_TOOL_QA)
    add_subdirectory(qa)
endif()
