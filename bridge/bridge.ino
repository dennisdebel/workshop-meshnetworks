// see: https://github.com/alteriom/painlessmesh/blob/main/USER_GUIDE.md

#include "painlessMesh.h"

#define MESH_PREFIX     "MyMeshNetwork"
#define MESH_PASSWORD   "somethingSneaky"
#define ROUTER_SSID     "YourRouterSSID"
#define ROUTER_PASSWORD "YourRouterPassword"

Scheduler userScheduler;
painlessMesh mesh;

void setup() {
  Serial.begin(115200);
  
  // Automatically detects router channel and configures mesh
  mesh.initAsBridge(MESH_PREFIX, MESH_PASSWORD,
                    ROUTER_SSID, ROUTER_PASSWORD,
                    &userScheduler, 5555);
  
  mesh.onReceive(&receivedCallback);
}

void loop() {
  mesh.update();
}

void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("Mesh message from %u: %s\n", from, msg.c_str());
  // Forward to Internet services (HTTP, MQTT, etc.)
}
