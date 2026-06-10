package ai.edgez.server.lwm2m;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;

import ai.edgez.server.lwm2m.wasm.WasmRuntimeService;

@SpringBootApplication
public class EdgeApplication {
    public static void main(String[] args) {
        SpringApplication.run(EdgeApplication.class, args);
        registerModules();
    }

    private static void registerModules() {
        // Use empty string for repoUrl to resolve from local repository only
        // WasmRuntimeService.registerGroupModule("12345", "ai.edgez.examples", "hello-group", "0.0.1-SNAPSHOT", "wasm", "wasm", "", "");
        // WasmRuntimeService.registerDeviceModule("12345-7C2C67FFFE53", "ai.edgez.examples", "hello-wasm", "0.0.1-SNAPSHOT", "wasm", "wasm", "", "");
    }
}
