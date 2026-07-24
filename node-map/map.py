import serial
import os
import json
import networkx as nx
from pyvis.network import Network


SERIAL_PORT = '/dev/cu.usbserial-A5069RR4'
BAUD_RATE = 115200

POSITION_FILE = "mesh_positions.json"


def load_positions():

    if os.path.exists(POSITION_FILE):
        with open(POSITION_FILE,"r") as f:
            return json.load(f)

    return {}



def save_positions(pos):

    with open(POSITION_FILE,"w") as f:
        json.dump(pos,f,indent=2)



def parse_mesh(node, graph):

    current = str(node["nodeId"])

    graph.add_node(
        current,
        label=current
    )


    for link in node.get("links", []):

        target=str(link["nodeId"])
        rssi=link.get("rssi",-100)


        graph.add_node(
            target,
            label=target
        )


        if rssi > 50:
            color="green"
        elif rssi > 30:
            color="orange"
        else:
            color="red"


        graph.add_edge(
            current,
            target,
            rssi=rssi,
            title=f"{rssi} dBm",
            color=color
        )



last_topology=None


def generate_map(json_data):

    global last_topology


    if json_data == last_topology:
        return


    last_topology=json_data


    data=json.loads(json_data)


    G=nx.Graph()

    parse_mesh(
        data,
        G
    )


    positions=load_positions()


    net=Network(
        height="750px",
        width="100%",
        notebook=False
    )


    net.from_nx(G)



    # Restore previous positions

    for node in net.nodes:

        nid=str(node["id"])

        if nid in positions:

            node["x"]=positions[nid]["x"]
            node["y"]=positions[nid]["y"]

            node["physics"]=False



    # physics only for new nodes

    net.set_options("""
{
 "physics":{
   "enabled":true,
   "solver":"forceAtlas2Based",
   "forceAtlas2Based":{
      "gravitationalConstant":-80,
      "centralGravity":0.01,
      "springLength":150,
      "springConstant":0.05
   },
   "stabilization":{
      "enabled":true,
      "iterations":200
   }
 }
}
""")


    net.save_graph(
        "mesh_map.html"
    )


    # inject position saver + live reload

    with open("mesh_map.html","r") as f:
        html=f.read()



    html=html.replace(
    "</body>",
    """

<script>


network.once(
"stabilizationIterationsDone",
function(){

    let pos=network.getPositions();


    fetch("positions.json",
    {
        method:"POST",
        body:JSON.stringify(pos)
    });

});


setTimeout(
function(){
location.reload();
},
5000);


</script>


</body>

"""
)



    with open("mesh_map.html","w") as f:
        f.write(html)



    print("map updated")





print("Listening:",SERIAL_PORT)


with serial.Serial(
    SERIAL_PORT,
    BAUD_RATE,
    timeout=1
) as ser:


    while True:

        line=ser.readline().decode(
            "utf-8",
            errors="ignore"
        ).strip()


        if line.startswith("TOPOLOGY:"):

            generate_map(
                line.replace(
                    "TOPOLOGY:",
                    ""
                )
            )