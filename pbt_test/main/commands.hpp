#pragma once

#include <span>

#include "rpc_transport.hpp"

std::span<const rpc::command> get_commands();
