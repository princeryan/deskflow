# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

set(target ${CMAKE_PROJECT_NAME}-clipboard)

add_executable(${target} main.cpp)

set_target_properties(${target} PROPERTIES AUTOMOC ON)

target_link_libraries(${target} Qt6::Gui)

install(
  TARGETS ${target}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
