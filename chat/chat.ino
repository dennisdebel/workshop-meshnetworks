#include <ESP8266WiFi.h>
#include "painlessMesh.h"


// -----------------------
// Set your nickname!
// -----------------------

String nickname = "DENNIS";


// remember received messages

#define MAX_MESSAGES 50
String seenMessages[MAX_MESSAGES];
int seenIndex = 0;

Scheduler userScheduler;


// -----------------------
// painlessMesh settings
// -----------------------

#define MESH_PREFIX     "chat"
#define MESH_PASSWORD   "12345678"
#define MESH_PORT       5555

painlessMesh mesh;

// -----------------------
// Send chat
// -----------------------

void sendChat(String text)
{
  String id = String(millis());
  
  // remember own message

  seenMessages[seenIndex] = id;
  seenIndex++;
  if (seenIndex >= MAX_MESSAGES)
    seenIndex = 0;

  String path =
    String(mesh.getNodeId());

  String packet =
    "CHAT|" +
    id +
    "|" +
    nickname +
    "|" +
    path +
    "|" +
    text;

  Serial.println("Sending:");
  Serial.println(packet);

  mesh.sendBroadcast(packet);

}


// -----------------------
// Receive chat
// -----------------------

void receivedCallback(uint32_t from, String &msg)
{
  if (!msg.startsWith("CHAT|"))
    return;

  msg.remove(0, 5);

  int p1 = msg.indexOf('|');
  int p2 = msg.indexOf('|', p1 + 1);
  int p3 = msg.indexOf('|', p2 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0)
    return;

  String id =
    msg.substring(0, p1);

  // duplicate protection

  for (int i = 0; i < MAX_MESSAGES; i++)
  {
    if (seenMessages[i] == id)
    {
      return;
    }
  }

  seenMessages[seenIndex] = id;

  seenIndex++;

  if (seenIndex >= MAX_MESSAGES)
    seenIndex = 0;

  String name =
    msg.substring(
      p1 + 1,
      p2
    );

  String path =
    msg.substring(
      p2 + 1,
      p3
    );

  String text =
    msg.substring(
      p3 + 1
    );

  // calculate hops

  int hops = 0;

  for (int i = 0; i < path.length(); i++)
  {
    if (path[i] == ',')
      hops++;
  }

  Serial.println();
  Serial.println("------ CHAT ------");

  Serial.print("FROM: ");
  Serial.println(name);

  Serial.print("PATH: ");
  Serial.println(path);

  Serial.print("HOPS: ");
  Serial.println(hops);

  Serial.print("MESSAGE: ");
  Serial.println(text);

  Serial.println("------------------");

  // blink reception

  digitalWrite(
    LED_BUILTIN,
    LOW
  );

  delay(50);

  digitalWrite(
    LED_BUILTIN,
    HIGH
  );

  // append ourselves to path

  String newPath =
    path +
    "," +
    String(mesh.getNodeId());

  String forward =
    "CHAT|" +
    id +
    "|" +
    name +
    "|" +
    newPath +
    "|" +
    text;

  mesh.sendBroadcast(
    forward
  );

}

// -----------------------
// Setup
// -----------------------

void setup() {

  Serial.begin(115200);

  mesh.init(
    MESH_PREFIX,
    MESH_PASSWORD,
    &userScheduler,
    MESH_PORT
  );

  pinMode(
    LED_BUILTIN,
    OUTPUT
  );

  // LED off
  digitalWrite(
    LED_BUILTIN,
    HIGH
  );

  mesh.onReceive(
    &receivedCallback
  );

  mesh.onNewConnection([](uint32_t nodeId) {
    Serial.print("NEW CONNECTION: ");
    Serial.println(nodeId);
  });


  mesh.onChangedConnections([]() {
    Serial.println("CONNECTIONS CHANGED");
    Serial.print("Nodes: ");
    Serial.println(
      mesh.getNodeList().size()
    );
  });
}

// -----------------------
// Loop
// -----------------------

void loop() {
  mesh.update();

  if (Serial.available())
  {
    String text =
      Serial.readStringUntil('\n'); // Read serial until receiving a newline (enter)

    text.trim();

    if (text.length() > 0)
    {
      sendChat(text);
    }

  }

}
