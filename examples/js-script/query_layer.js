// Example JavaScript script to query LayerMeta information
// This script demonstrates how to access USD Layer metadata through the core JavaScript example

console.log("=== USD Layer Metadata Query Script ===");
console.log("This script demonstrates querying LayerMetas through the core JavaScript example.");
console.log("");

// Get the LayerMetas object from the C++ side
var layerMetas = getLayerMetas();

if (layerMetas === null) {
    console.log("ERROR: No LayerMetas available");
} else {
    console.log("LayerMetas successfully retrieved!");
    console.log("");
    
    // Display basic metadata
    console.log("=== Basic Layer Metadata ===");
    console.log("Up Axis: " + layerMetas.upAxis);
    console.log("Default Prim: " + layerMetas.defaultPrim);
    console.log("Meters Per Unit: " + layerMetas.metersPerUnit);
    console.log("Time Codes Per Second: " + layerMetas.timeCodesPerSecond);
    console.log("Frames Per Second: " + layerMetas.framesPerSecond);
    console.log("Kilograms Per Unit: " + layerMetas.kilogramsPerUnit);
    console.log("");
    
    // Display time range information
    console.log("=== Time Range ===");
    console.log("Start Time Code: " + layerMetas.startTimeCode);
    if (layerMetas.endTimeCode === null) {
        console.log("End Time Code: (infinite/not set)");
    } else {
        console.log("End Time Code: " + layerMetas.endTimeCode);
    }
    console.log("");
    
    // Display documentation and comments
    console.log("=== Documentation ===");
    if (layerMetas.comment && layerMetas.comment.length > 0) {
        console.log("Comment: " + layerMetas.comment);
    } else {
        console.log("Comment: (none)");
    }
    
    if (layerMetas.doc && layerMetas.doc.length > 0) {
        console.log("Documentation: " + layerMetas.doc);
    } else {
        console.log("Documentation: (none)");
    }
    console.log("");
    
    // Display USDZ-specific metadata
    console.log("=== USDZ Extensions ===");
    console.log("Auto Play: " + layerMetas.autoPlay);
    console.log("Playback Mode: " + layerMetas.playbackMode);
    console.log("");
    
    // Display sub-layers information
    console.log("=== Sub-Layers ===");
    if (layerMetas.subLayers.length > 0) {
        console.log("Number of sub-layers: " + layerMetas.subLayers.length);
        for (var i = 0; i < layerMetas.subLayers.length; i++) {
            var subLayer = layerMetas.subLayers[i];
            console.log("  Sub-layer " + i + ":");
            console.log("    Asset Path: " + subLayer.assetPath);
            console.log("    Layer Offset: " + subLayer.layerOffset.offset);
            console.log("    Layer Scale: " + subLayer.layerOffset.scale);
        }
    } else {
        console.log("No sub-layers defined");
    }
    console.log("");
    
    // Display root prim children
    console.log("=== Root Prim Children ===");
    if (layerMetas.primChildren.length > 0) {
        console.log("Number of root prims: " + layerMetas.primChildren.length);
        for (var i = 0; i < layerMetas.primChildren.length; i++) {
            console.log("  Prim " + i + ": " + layerMetas.primChildren[i]);
        }
    } else {
        console.log("No root prims defined");
    }
    console.log("");
    
    // Perform some analysis
    console.log("=== Analysis ===");
    
    // Check if this looks like a typical scene setup
    var hasDefaultPrim = layerMetas.defaultPrim && layerMetas.defaultPrim.length > 0;
    var hasTimeRange = layerMetas.startTimeCode !== undefined && layerMetas.endTimeCode !== null;
    var isAnimated = hasTimeRange && (layerMetas.endTimeCode > layerMetas.startTimeCode);
    
    console.log("Has default prim: " + hasDefaultPrim);
    console.log("Has time range: " + hasTimeRange);
    console.log("Is animated: " + isAnimated);
    
    if (layerMetas.upAxis !== "Y") {
        console.log("NOTE: This USD file uses " + layerMetas.upAxis + " as up-axis (not Y)");
    }
    
    if (layerMetas.metersPerUnit !== 1.0) {
        console.log("NOTE: This USD file has custom scale: " + layerMetas.metersPerUnit + " meters per unit");
    }
    
    if (layerMetas.subLayers.length > 0) {
        console.log("NOTE: This USD file references " + layerMetas.subLayers.length + " sub-layer(s)");
    }
    
    console.log("");
    console.log("=== JSON Representation ===");
    console.log(JSON.stringify(layerMetas, null, 2));
}

console.log("");
console.log("=== Script Complete ===");