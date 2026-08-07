function updateStatus()
{

    fetch("/node-id")
    .then(r=>r.text())
    .then(id =>
    {
        document.getElementById("myNode").innerHTML=id;
    });


    fetch("/nodes")
    .then(r=>r.json())
    .then(nodes =>
    {

        let div=document.getElementById("nodes");

        div.innerHTML="";


        if(nodes.length==0)
        {
            div.innerHTML="no remote nodes";
            return;
        }


        nodes.forEach(id =>
        {

            let link=document.createElement("a");

            link.href="#";

            link.innerHTML=id;

            link.onclick=function()
            {
                loadNode(
                    id,
                    "/index.html"
                );

                return false;
            };


            div.appendChild(link);

            div.appendChild(
                document.createElement("br")
            );

        });

    })

    .catch(err =>
    {
        console.log("node update error:",err);
    });

}



// function loadNode(id)
// {
//     console.log("Loading node",id);

//     document.getElementById("content").innerHTML =
//         "Loading node "+id+"...";
// }



// load content from other node
// function loadNode(node,path)
// {
//     console.log("LOADING", node, path);

//     fetch("/mesh/" + node + path)
//     .then(r=>r.text())
//     .then(html =>
//     {
//         console.log(html);

//         document.open();
//         document.write(html);
//         document.close();
//     });
// }
function loadNode(node,path)
{

    fetch(
        "/mesh/" + node + path
    )

    .then(r => r.text())
    .then(html =>
    {

        html = html.replaceAll(
            'href="style.css"',
            'href="/mesh/'+node+'/style.css"'
        );

        html = html.replaceAll(
            'src="app.js"',
            'src="/mesh/'+node+'/app.js"'
        );


        document.open();

        html = html.replace(
            "<head>",
            "<head><base href=\"/mesh/"+node+"/\">"
        );

        document.write(html);
        document.close();

    });

}


// update immediately
updateStatus();


// refresh every 2 seconds
setInterval(
    updateStatus,
    4000
);
//let myNode = "2882848806";


// document.getElementById("myNode").innerHTML = myNode;


// function loadNodes()
// {

//     fetch("/nodes")

//     .then(r=>r.json())

//     .then(nodes =>
//     {

//         let div=document.getElementById("nodes");

//         div.innerHTML="";


//         nodes.forEach(node =>
//         {

//             addNode(node);

//         });

//     });

// }


// loadNodes();


// function addNode(id)
// {

//     let div=document.getElementById("nodes");


//     let link=document.createElement("a");

//     link.href="#";

//     link.innerHTML=id;


//     link.onclick=function()
//     {
//         loadNode(
//             id,
//             "/index.html"
//         );

//         return false;
//     };


//     div.appendChild(link);

//     div.appendChild(
//         document.createElement("br")
//     );
// }


// function loadNode(node,path)
// {
//     document.getElementById("content").src =
//         "/mesh/" + node + path;
// }

