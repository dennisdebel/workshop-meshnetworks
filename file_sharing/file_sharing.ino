//#include <ESP8266mDNS.h>
#include "./DNSServer.h" // Dns server, needed for mdns!
#include "painlessMesh.h"

#include <Arduino.h>
#include <LittleFS.h>

#ifdef ESP8266
#include <ESPAsyncTCP.h>
#else
#include <AsyncTCP.h>
#endif

#include <ESPAsyncWebServer.h>

// -----------------------
// dns hijjacking 
// -----------------------

DNSServer dnsServer;
const byte DNS_PORT = 53;
  
// -----------------------
// painlessMesh settings
// -----------------------

#define MESH_PREFIX     "mesh" // the wifi ssid needs to be the same for each node
#define MESH_PASSWORD   "12345678"
#define MESH_PORT       5555
 
#define CHUNK_SIZE 256 // files need to be transfered from the file system to RAM before sending, so we need to chunk larger files to not overflow the RAM.

struct IncomingFile
{
  String name;
  size_t size;
  size_t received;
  File file;
};

IncomingFile incoming;


painlessMesh mesh;

AsyncWebServer server(80);

Scheduler userScheduler;


// -----------------------
// Has to prevent dupes
// -----------------------
bool hasFile(String name)
{
    return LittleFS.exists(name);
}


// -----------------------
// Base64 en/decoder (for images etc)
// -----------------------

const char b64chars[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


int base64Decode(
    String input,
    uint8_t *output
)
{
    int val = 0;
    int valb = -8;
    int outLen = 0;

    for (int i = 0; i < input.length(); i++)
    {
        char c = input[i];

        if (c == '=')
            break;

        int idx = -1;

        for(int j=0;j<64;j++)
        {
            if(b64chars[j] == c)
            {
                idx=j;
                break;
            }
        }

        if(idx < 0)
            continue;

        val = (val << 6) + idx;
        valb += 6;

        if(valb >= 0)
        {
            output[outLen++] =
                (val >> valb) & 0xFF;

            valb -= 8;
        }
    }

    return outLen;
}


String base64Encode(uint8_t *data, size_t len)
{
    String output;

    int i = 0;

    while (i < len)
    {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;


        uint32_t triple =
            (octet_a << 16) |
            (octet_b << 8) |
            octet_c;

        output += b64chars[(triple >> 18) & 0x3F];
        output += b64chars[(triple >> 12) & 0x3F];

        if (i - 2 < len)
            output += b64chars[(triple >> 6) & 0x3F];
        else
            output += '=';

        if (i - 1 < len)
            output += b64chars[triple & 0x3F];
        else
            output += '=';
    }

    return output;
}

// -----------------------
// Mesh callback
// -----------------------

void receivedCallback(uint32_t from, String &msg)
{

Serial.printf("RX %u: %s\n",from,msg.c_str());


if(msg=="FILE_LIST")
{
    Dir dir = LittleFS.openDir("/");

    while(dir.next())
    {
        String reply="FILE:";
        reply += dir.fileName();
        reply += ":";
        reply += dir.fileSize();

        mesh.sendSingle(from,reply);
    }

    return;
}


if(msg.startsWith("FILE:"))
{

    int p1 = msg.indexOf(':',5);

    String filename =
        msg.substring(5,p1);

    size_t filesize =
        msg.substring(p1+1).toInt();

    Serial.print("Remote file: ");
    Serial.print(filename);
    Serial.print(" ");
    Serial.println(filesize);

    if(!hasFile(filename))
    {

        Serial.println("Missing, requesting");

        mesh.sendSingle(
            from,
            "GET_FILE:" + filename
        );

    }
    else
    {
        Serial.println("Already have it");
    }

    return;
}

if(msg.startsWith("GET_FILE:"))
{
    String filename=msg.substring(9);

    File f=LittleFS.open(filename,"r");

    if(!f)
    {
        Serial.println("missing file");
        return;
    }


    String start="FILE_START:";
    start+=filename;
    start+=":";
    start+=f.size();

    mesh.sendSingle(from,start);

    size_t pos=0;

    while(f.available())
    {
        uint8_t buffer[CHUNK_SIZE];
        
        int len = f.read(buffer, CHUNK_SIZE);
        
        String chunk="FILE_CHUNK:";
        chunk += filename;
        chunk += ":";
        chunk += pos;
        chunk += ":";
        
        String encoded = base64Encode(buffer, len);
        
        chunk += encoded;
        
        mesh.sendSingle(from, chunk);

        pos+=len;

        delay(5);
    }


    f.close();

    String end="FILE_END:";
    end+=filename;

    mesh.sendSingle(from,end);

    Serial.println("file sent");

    return;
}


if(msg.startsWith("FILE_START:"))
{
    int p1=msg.indexOf(':',11);

    incoming.name=
        msg.substring(11,p1);

    incoming.size=
        msg.substring(p1+1).toInt();

    incoming.received=0;

    incoming.file=
        LittleFS.open(
            incoming.name,
            "w"
        );

    Serial.println("Receiving:");
    Serial.println(incoming.name);

    return;
}


if(msg.startsWith("FILE_CHUNK:"))
{

    int p1 = msg.indexOf(':',11);
    int p2 = msg.indexOf(':',p1+1);

    String filename =
        msg.substring(11,p1);

    int offset =
        msg.substring(p1+1,p2).toInt();

    String encoded =
        msg.substring(p2+1);

    uint8_t decoded[400];

    int decodedLen =
        base64Decode(
            encoded,
            decoded
        );

    if(incoming.file)
    {
        incoming.file.seek(offset);

        incoming.file.write(
            decoded,
            decodedLen
        );

        incoming.received += decodedLen;
    }

    return;
}


  if(msg.startsWith("FILE_END:"))
  {
  
      if(incoming.file)
      {
          incoming.file.close();
      }
  
      Serial.println("Finished:");
      Serial.println(incoming.name);
  
      // tell the whole mesh we now have this file
      mesh.sendBroadcast(
          "NEW_FILE:" + incoming.name
      );
  
      return;
  }

  if(msg.startsWith("NEW_FILE:"))
    {
        String filename = msg.substring(9);
    
        Serial.println("New file announced:");
        Serial.println(filename);
    
        mesh.sendBroadcast("FILE_LIST");
    
        return;
    }

}
// -----------------------
// Upload handler
// -----------------------

void handleUpload(
  AsyncWebServerRequest *request,
  String filename,
  size_t index,
  uint8_t *data,
  size_t len,
  bool final
)
{

  static File uploadFile;

  if(index == 0)
  {
    Serial.println("Upload start: " + filename);

    uploadFile = LittleFS.open(
      "/" + filename,
      "w"
    );
  }


  if(uploadFile)
  {
    uploadFile.write(data, len);
  }


  if(final)
  {
    uploadFile.close();

    Serial.println("Upload finished");
  }
}

///debug send file helper
void sendFile(uint32_t destination, String filename)
{
  File file = LittleFS.open(filename, "r");

  if(!file)
  {
    Serial.println("File not found");
    return;
  }


  String content = file.readString();

  file.close();

  String msg = "FILE:";
  msg += filename;
  msg += ":";
  msg += content;

  mesh.sendSingle(
    destination,
    msg
  );

  Serial.println("Sent:");
  Serial.println(msg);
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

  // Filesystem

  if(!LittleFS.begin())
  {
    Serial.println("LittleFS failed!");
    return;
  }

  Serial.println("LittleFS OK");

  // Print files

  Dir dir = LittleFS.openDir("/");

  while(dir.next())
  {
    Serial.print("FILE: ");
    Serial.print(dir.fileName());
    Serial.print(" SIZE: ");
    Serial.println(dir.fileSize());
  }


  // Mesh

  mesh.setDebugMsgTypes(
    ERROR |
    STARTUP |
    CONNECTION 
  );


//  mesh.init(
//    MESH_PREFIX,
//    MESH_PASSWORD,
//    MESH_PORT,
//    WIFI_AP,
//    6
//  );

  mesh.init(
      MESH_PREFIX,
      MESH_PASSWORD,
      &userScheduler,
      MESH_PORT
  );
  

  
  Serial.println("Mesh started");

  
  Serial.print("AP SSID: ");
  Serial.println(MESH_PREFIX);
  
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());


  Serial.print("INTERNAL AP: ");
  Serial.println(WiFi.softAPSSID());

  

 
  mesh.onReceive(
    &receivedCallback
  );


  mesh.onNewConnection([](uint32_t nodeId){
      Serial.printf("NEW CONNECTION: %u\n", nodeId);
  });
  
  
  mesh.onChangedConnections([](){
  
      Serial.println("CONNECTIONS CHANGED");
  
      SimpleList<uint32_t> nodes = mesh.getNodeList();
  
      Serial.print("Nodes: ");
  
      for(auto node : nodes)
      {
          Serial.print(node);
          Serial.print(" ");
      }
  
      Serial.println();
  
  });


  // -----------------------
  // Web root
  // -----------------------

  server.on("/", HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    String html;

    html += "<html><body>";
    html += "<h1>Mesh File Sharing</h1>";


    Dir dir = LittleFS.openDir("/");


    while(dir.next())
    {
      String name = dir.fileName();

      html += "<p>";
      html += name;
      html += " (";
      html += dir.fileSize();
      html += " bytes) ";

      html += "<a href='/download?file=";
      html += name;
      html += "'>download</a>";

      html += "</p>";
    }


    html += "<hr>";

    html += "<h2>Upload (< 100 kb!)</h2>";

    html += "<form method='POST' action='/upload' "
            "enctype='multipart/form-data'>";

    html += "<input type='file' name='upload'>";

    html += "<input type='submit'>";

    html += "</form>";


    html += "</body></html>";

    request->send(
      200,
      "text/html",
      html
    );

  });



  // -----------------------
  // Download
  // -----------------------

  server.on("/download",
  HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    if(!request->hasParam("file"))
    {
      request->send(
        400,
        "text/plain",
        "missing file"
      );

      return;
    }


    String filename =
      request->getParam("file")->value();


    request->send(
      LittleFS,
      filename,
      "text/plain"
    );

  });



  // -----------------------
  // Upload
  // -----------------------

  server.on(
    "/upload",
    HTTP_POST,

    [](AsyncWebServerRequest *request)
    {
      request->redirect("/");
    },

    handleUpload
  );



  server.begin();

  Serial.println("HTTP server started");



  // Set a custom hostname for the device
//  bool success = mesh.setHostname("mesh");
//    if (success) {
//      Serial.println("Hostname set successfully.");
//    } else {
//      Serial.println("Failed to set hostname.");
//    }
//  

  delay(1000); // maybe not needed

 dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); // redirect dns request to AP ip

 


} // end of setup()



// -----------------------
// Loop
// -----------------------

void loop()
{
  mesh.update();
  dnsServer.processNextRequest();

  //userScheduler.execute();

//  static uint32_t lastRequest = 0;
//  
//  if(mesh.getNodeList().size()>0 &&
//     millis()-lastRequest > 30000) // request every 30 seconds
//  {
//      lastRequest = millis();
//  
//      uint32_t node =
//        mesh.getNodeList().front();
//  
//      mesh.sendSingle(
//        node,
//        "GET:/newfile1.txt"
//      );
//  }

static unsigned long syncTimer=0;


    if(millis()-syncTimer>30000)
    {
        syncTimer=millis();
    
    
        if(mesh.getNodeList().size()>0)
        {
    
//            uint32_t node =
//                mesh.getNodeList().front();
//    
//    
//            mesh.sendSingle(
//                node,
//                "FILE_LIST"
//            );
            mesh.sendBroadcast("FILE_LIST");
    
            Serial.println("Requesting file list");
    
        }
    }

    static unsigned long last = 0;
  
    if (millis() - last > 3000)
    {
      last = millis();
  
      Serial.println("----");
      Serial.print("Node ID: ");
      Serial.println(mesh.getNodeId());
  
      Serial.print("Connections: ");
      Serial.println(mesh.getNodeList().size());
  
      for(auto node : mesh.getNodeList())
      {
        Serial.print("Connected: ");
        Serial.println(node);
      }
    }
}
