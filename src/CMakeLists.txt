idf_component_register(SRCS "main.c" "identity_manager.c" "ipc_manager.c" "hal_shift_registers.c" "core1_engine.c" "network_manager.c"
                    INCLUDE_DIRS "." "../include"
                    REQUIRES cjson)
