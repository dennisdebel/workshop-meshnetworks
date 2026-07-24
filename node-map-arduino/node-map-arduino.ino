#include "painlessMesh.h"
#include <ArduinoJson.h>


// -----------------------
// painlessMesh settings
// -----------------------

#define MESH_PREFIX     "whateverYouLike" // do these all need to be the same? i think so...
#define MESH_PASSWORD   "somethingSneaky"
#define MESH_PORT       5555
 


painlessMesh mesh;


Scheduler userScheduler;


void printTopology()
{
    String json;

    json += "{";
    json += "\"nodeId\":";
    json += mesh.getNodeId();
    json += ",\"links\":[";


    bool first=true;

    for(auto node : mesh.getNodeList())
    {
        if(!first)
            json += ",";

        first=false;


        json += "{";
        json += "\"nodeId\":";
        json += node;
        json += ",";

        // temporary RSSI
        json += "\"rssi\":";
        json += WiFi.RSSI();

        json += "}";
    }


    json += "]}";


    Serial.print("TOPOLOGY:");
    Serial.println(json);
}



// -----------------------
// Setup
// -----------------------

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("Starting...");





  // Mesh

  mesh.setDebugMsgTypes(
    ERROR |
    STARTUP |
    CONNECTION 
  );



  mesh.init(
      MESH_PREFIX,
      MESH_PASSWORD,
      &userScheduler,
      MESH_PORT
  );



//  mesh.onReceive(
//    &receivedCallback
//  );


  mesh.onNewConnection([](uint32_t nodeId){
    int rssi = WiFi.RSSI();

    Serial.print("LINK:");
    Serial.print(nodeId);
    Serial.print(",");

    Serial.print(rssi);
    Serial.println("dBm");

  });
  
  
  mesh.onChangedConnections([](){

  });



}




// -----------------------
// Loop
// -----------------------

void loop()
{
    mesh.update();
    //userScheduler.execute();

  static uint32_t lastTopology = 0;
  
  if(millis()-lastTopology > 5000)
  {
     lastTopology = millis();
  
     printTopology();
  }
}
