#ifndef _GRENDER_SHADERS_H_
#define _GRENDER_SHADERS_H_

/**
 * WGSL for both draw passes. Everything is vertex-pulled from storage buffers:
 * nodes and edges are drawn as instanced quads (6 vertices, 2 triangles) with
 * no vertex buffers, so a frame is exactly two draw calls regardless of graph
 * size. Layout must match grGlobalsUBO and the bind group in grRenderer.c.
 */
static const char GR_WGSL_SOURCE[] =
    "struct Globals {\n"
    "  viewProj   : mat4x4f,\n"
    "  camRight   : vec4f,\n"
    "  camUp      : vec4f,\n"
    "  viewport   : vec2f,\n"
    "  posDim     : u32,\n"
    "  flags      : u32,\n"
    "  nodeFill   : vec4f,\n"
    "  nodeStroke : vec4f,\n"
    "  nodeParams : vec4f,\n" // x radius, y strokeWidth, z sizeMode, w proj11
    "  nodeSizeLimits : vec4f,\n" // x minPixelRadius, y maxPixelRadius (0 = off)
    "  edgeColor  : vec4f,\n"
    // x width, y sizeMode, z maxDegree (degree-alpha), w mean edge weight
    "  edgeParams : vec4f,\n"
    "}\n"
    "\n"
    "const FLAG_NODE_COLORS      : u32 = 1u;\n"
    "const FLAG_NODE_SIZES       : u32 = 2u;\n"
    "const FLAG_EDGE_COLORS      : u32 = 4u;\n"
    "const FLAG_EDGE_DEGREE_ALPHA : u32 = 8u;\n"
    "const FLAG_EDGE_WEIGHT_WIDTH : u32 = 16u;\n"
    "const FLAG_DIRECTED         : u32 = 32u;\n"
    "const FLAG_NODE_ROUNDED_SQUARE : u32 = 64u;\n"
    "const FLAG_EDGE_DASHED      : u32 = 128u;\n"
    "const FLAG_NODE_DEGREE_SCALE : u32 = 256u;\n"
    "\n"
    // Arrowhead size relative to the edge's own drawn half-width, so a
    // thicker edge (wider edgeStyle.width or a weight-scaled edge) grows a
    // proportionally bigger arrowhead instead of swallowing a fixed-size one.
    "const ARROW_LENGTH_SCALE : f32 = 11.0;\n"
    "const ARROW_WIDTH_SCALE  : f32 = 4.5;\n"
    "\n"
    "@group(0) @binding(0) var<uniform> G : Globals;\n"
    "@group(0) @binding(1) var<storage, read> positions  : array<f32>;\n"
    "@group(0) @binding(2) var<storage, read> nodeIds    : array<u32>;\n"
    "@group(0) @binding(3) var<storage, read> nodeColors : array<u32>;\n"
    "@group(0) @binding(4) var<storage, read> nodeSizes  : array<f32>;\n"
    "@group(0) @binding(5) var<storage, read> edges      : array<u32>;\n"
    "@group(0) @binding(6) var<storage, read> edgeColors : array<u32>;\n"
    "@group(0) @binding(7) var<storage, read> nodeDegrees : array<u32>;\n"
    "@group(0) @binding(8) var<storage, read> edgeWeights : array<f32>;\n"
    // 0/1 per edge, edge-buffer order (see grRendererSetEdgeDashed); only
    // read when FLAG_EDGE_DASHED is set, exactly like every other optional
    // per-element attribute buffer here.
    "@group(0) @binding(9) var<storage, read> edgeDashed : array<u32>;\n"
    "\n"
    "fn getPos(i : u32) -> vec3f {\n"
    "  let base = i * G.posDim;\n"
    "  var p = vec3f(positions[base], positions[base + 1u], 0.0);\n"
    "  if (G.posDim >= 3u) { p.z = positions[base + 2u]; }\n"
    "  return p;\n"
    "}\n"
    "\n"
    // Screen pixels covered by one world unit at clip-space depth w.
    "fn pxPerWorld(clipW : f32) -> f32 {\n"
    "  return G.nodeParams.w * G.viewport.y / (2.0 * max(clipW, 1e-6));\n"
    "}\n"
    "\n"
    "fn unpackColor(c : u32) -> vec4f {\n"
    "  return vec4f(\n"
    "    f32(c & 0xFFu) / 255.0,\n"
    "    f32((c >> 8u) & 0xFFu) / 255.0,\n"
    "    f32((c >> 16u) & 0xFFu) / 255.0,\n"
    "    f32((c >> 24u) & 0xFFu) / 255.0);\n"
    "}\n"
    "\n"
    // Signed distance from p to a square of half-size (halfSize + r) with
    // corners rounded to radius r, centered at the origin. Standard
    // rounded-box SDF (Inigo Quilez).
    "fn sdRoundBox(p : vec2f, halfSize : vec2f, r : f32) -> f32 {\n"
    "  let q = abs(p) - halfSize + vec2f(r, r);\n"
    "  return length(max(q, vec2f(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "\n"
    // Quad corners for two CCW triangles.
    "const CORNERS = array<vec2f, 6>(\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));\n"
    "\n"
    // ---------------------------------------------------------------- nodes --
    "struct NodeOut {\n"
    "  @builtin(position) clip : vec4f,\n"
    "  @location(0) local : vec2f,\n"      // px offset from node center
    "  @location(1) fill : vec4f,\n"
    "  @location(2) @interpolate(flat) radiusPx : f32,\n"
    "  @location(3) @interpolate(flat) strokePx : f32,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsNode(@builtin(vertex_index) vid : u32,\n"
    "          @builtin(instance_index) iid : u32) -> NodeOut {\n"
    "  let corner = CORNERS[vid];\n"
    "  let id = nodeIds[iid];\n"
    "  var clip = G.viewProj * vec4f(getPos(id), 1.0);\n"
    "\n"
    "  var radius = G.nodeParams.x;\n"
    "  if ((G.flags & FLAG_NODE_DEGREE_SCALE) != 0u) {\n"
    "    radius = radius * (1.0 + G.nodeSizeLimits.w * sqrt(f32(nodeDegrees[id])));\n"
    "  }\n"
    "  if ((G.flags & FLAG_NODE_SIZES) != 0u) { radius = nodeSizes[id]; }\n"
    "  var radiusPx = radius;\n"
    "  if (G.nodeParams.z > 0.5) {\n"
    "    radiusPx = radius * pxPerWorld(clip.w);\n"
    // Clamp only the on-screen pixel size, at draw time; the world-space
    // radius (e.g. used as a physics collision radius elsewhere) is
    // untouched. maxPixelRadius <= 0 means no ceiling.
    "    if (G.nodeSizeLimits.y > 0.0) { radiusPx = min(radiusPx, G.nodeSizeLimits.y); }\n"
    "    radiusPx = max(radiusPx, G.nodeSizeLimits.x);\n"
    "  }\n"
    "  let strokePx = select(G.nodeParams.y,\n"
    "                        G.nodeParams.y * pxPerWorld(clip.w),\n"
    "                        G.nodeParams.z > 0.5);\n"
    "\n"
    // 1px feather margin so antialiasing is never clipped by the quad.
    "  let quadPx = radiusPx + strokePx + 1.0;\n"
    "  let local = corner * quadPx;\n"
    "  clip = vec4f(clip.xy + local * 2.0 / G.viewport * clip.w, clip.zw);\n"
    "\n"
    "  var fill = G.nodeFill;\n"
    "  if ((G.flags & FLAG_NODE_COLORS) != 0u) {\n"
    "    fill = unpackColor(nodeColors[id]);\n"
    "  }\n"
    "  return NodeOut(clip, local, fill, radiusPx, strokePx);\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fsNode(in : NodeOut) -> @location(0) vec4f {\n"
    // sdf: signed distance from the fragment to the node's fill boundary
    // (negative inside, 0 on the boundary, positive outside) -- 0 for the
    // circle case reduces to the same length(in.local) - radiusPx used
    // before this was unified with the rounded-square case below, so
    // existing (circle) visuals are unchanged.
    "  var sdf : f32;\n"
    "  if ((G.flags & FLAG_NODE_ROUNDED_SQUARE) != 0u) {\n"
    "    let cr = clamp(G.nodeSizeLimits.z, 0.0, 0.5) * in.radiusPx;\n"
    "    let half = vec2f(in.radiusPx - cr, in.radiusPx - cr);\n"
    "    sdf = sdRoundBox(in.local, half, cr);\n"
    "  } else {\n"
    "    sdf = length(in.local) - in.radiusPx;\n"
    "  }\n"
    "  let coverage = 1.0 - smoothstep(in.strokePx - 1.0, in.strokePx, sdf);\n"
    "  if (coverage <= 0.0) { discard; }\n"
    "  var color = in.fill;\n"
    "  if (in.strokePx > 0.0) {\n"
    "    let t = smoothstep(-0.5, 0.5, sdf);\n"
    "    color = mix(in.fill, G.nodeStroke, t);\n"
    "  }\n"
    "  return vec4f(color.rgb, color.a * coverage);\n"
    "}\n"
    "\n"
    // ---------------------------------------------------------------- edges --
    "struct EdgeOut {\n"
    "  @builtin(position) clip : vec4f,\n"
    "  @location(0) across : f32,\n"         // signed px across the edge
    "  @location(1) color : vec4f,\n"
    "  @location(2) @interpolate(flat) halfWidthPx : f32,\n"
    "  @location(3) along : f32,\n"          // px along the edge from A, for dashing
    "  @location(4) @interpolate(flat) dashed : u32,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsEdge(@builtin(vertex_index) vid : u32,\n"
    "          @builtin(instance_index) iid : u32) -> EdgeOut {\n"
    "  let a = edges[iid * 2u];\n"
    "  let b = edges[iid * 2u + 1u];\n"
    "  var dashed = 0u;\n"
    "  if ((G.flags & FLAG_EDGE_DASHED) != 0u) { dashed = edgeDashed[iid]; }\n"
    "  var clipA = G.viewProj * vec4f(getPos(a), 1.0);\n"
    "  var clipB = G.viewProj * vec4f(getPos(b), 1.0);\n"
    // Clamp w to keep endpoints behind a perspective camera from exploding.
    "  clipA.w = max(clipA.w, 1e-4);\n"
    "  clipB.w = max(clipB.w, 1e-4);\n"
    "\n"
    "  let screenA = clipA.xy / clipA.w * G.viewport * 0.5;\n"
    "  let screenB = clipB.xy / clipB.w * G.viewport * 0.5;\n"
    "  var dir = screenB - screenA;\n"
    "  let len = length(dir);\n"
    "  if (len < 1e-6) { dir = vec2f(1.0, 0.0); } else { dir = dir / len; }\n"
    "  let normal = vec2f(-dir.y, dir.x);\n"
    "\n"
    // Width at the v end, used to size the arrowhead (when directed) and to
    // trim the shaft so it stops where the arrowhead begins, regardless of
    // which vertex (shaft or arrowhead) is being emitted below.
    "  var halfWidthAtB = G.edgeParams.x * 0.5;\n"
    "  if (G.edgeParams.y > 0.5) {\n"
    "    halfWidthAtB = halfWidthAtB * pxPerWorld(clipB.w);\n"
    "  }\n"
    "  if ((G.flags & FLAG_EDGE_WEIGHT_WIDTH) != 0u) {\n"
    "    halfWidthAtB = halfWidthAtB * (edgeWeights[iid] / max(G.edgeParams.w, 1e-6));\n"
    "  }\n"
    "  let isDirected = (G.flags & FLAG_DIRECTED) != 0u;\n"
    "  var arrowLen = 0.0;\n"
    "  var tipPullback = 0.0;\n"
    "  if (isDirected) {\n"
    "    arrowLen = halfWidthAtB * ARROW_LENGTH_SCALE;\n"
    // Same on-screen-radius computation as vsNode, for node b, so the
    // arrowhead's tip lands just outside the destination node's drawn
    // circle instead of underneath it (nodes draw on top of edges in 2D).
    "    var nodeRadiusAtB = G.nodeParams.x;\n"
    "    if ((G.flags & FLAG_NODE_DEGREE_SCALE) != 0u) {\n"
    "      nodeRadiusAtB = nodeRadiusAtB * (1.0 + G.nodeSizeLimits.w * sqrt(f32(nodeDegrees[b])));\n"
    "    }\n"
    "    if ((G.flags & FLAG_NODE_SIZES) != 0u) { nodeRadiusAtB = nodeSizes[b]; }\n"
    "    if (G.nodeParams.z > 0.5) {\n"
    "      tipPullback = nodeRadiusAtB * pxPerWorld(clipB.w);\n"
    "      if (G.nodeSizeLimits.y > 0.0) { tipPullback = min(tipPullback, G.nodeSizeLimits.y); }\n"
    "      tipPullback = max(tipPullback, G.nodeSizeLimits.x);\n"
    "    } else {\n"
    "      tipPullback = nodeRadiusAtB;\n"
    "    }\n"
    "  }\n"
    "  let arrowTip = screenB - dir * tipPullback;\n"
    "  let arrowBase = arrowTip - dir * arrowLen;\n"
    "\n"
    "  var color = G.edgeColor;\n"
    "  if ((G.flags & FLAG_EDGE_DEGREE_ALPHA) != 0u) {\n"
	"    color.a = 1.0;\n"
    "    let d = sqrt(f32(nodeDegrees[a]) * f32(nodeDegrees[b]));\n"
    "    let t = log2(d + 1.0) / log2(max(G.edgeParams.z, 1.0) + 1.0);\n"
    "    color.a *= mix(1.0, 0.01, pow(t, 0.1));\n"
    "  }\n"
    "  if ((G.flags & FLAG_EDGE_COLORS) != 0u) {\n"
    "    let ec = unpackColor(edgeColors[iid]);\n"
    "    if ((G.flags & FLAG_EDGE_DEGREE_ALPHA) != 0u) {\n"
    // Highlight fills every edge slot: base style for ordinary edges, override
    // color for highlighted ones. Only the override should beat degree-alpha.
    "      let diff = abs(ec.rgb - G.edgeColor.rgb);\n"
    "      if (any(diff > vec3f(2.0 / 255.0))) {\n"
    "        color = vec4f(ec.rgb, 1.0);\n"
    "      }\n"
    "    } else {\n"
    "      color = ec;\n"
    "    }\n"
    "  }\n"
    "\n"
    // vid 6..8: arrowhead triangle tipped just outside node b (arrowTip),
    // base pulled further back by arrowLen. across is held at 0 with a
    // small flat halfWidthPx so fsEdge's feathering is a no-op (full
    // coverage) over the whole triangle -- only ever reached when
    // isDirected (the CPU only issues 9 vertices per instance for directed
    // graphs).
    "  if (vid >= 6u) {\n"
    "    var pos = arrowTip;\n"
    "    if (vid == 7u) { pos = arrowBase - normal * halfWidthAtB * ARROW_WIDTH_SCALE; }\n"
    "    if (vid == 8u) { pos = arrowBase + normal * halfWidthAtB * ARROW_WIDTH_SCALE; }\n"
    "    let clip = vec4f(pos * 2.0 / G.viewport * clipB.w, clipB.zw);\n"
    "    return EdgeOut(clip, 0.0, color, 0.5, len, dashed);\n"
    "  }\n"
    "\n"
    // vid 0..5 -> (end, side): triangles (A-,B-,B+),(A-,B+,A+)
    "  var end = 0.0;\n"
    "  if (vid == 1u || vid == 2u || vid == 4u) { end = 1.0; }\n"
    "  var side = -1.0;\n"
    "  if (vid == 2u || vid == 4u || vid == 5u) { side = 1.0; }\n"
    "\n"
    "  let clipEnd = mix(clipA, clipB, end);\n"
    "  var halfWidthPx = G.edgeParams.x * 0.5;\n"
    "  if (G.edgeParams.y > 0.5) {\n"
    "    halfWidthPx = halfWidthPx * pxPerWorld(clipEnd.w);\n"
    "  }\n"
    // Thickness scales linearly with weight relative to the mean of all
    // uploaded weights, so relative thickness directly shows relative weight
    // (edgeParams.x/edgeStyle.width is the base thickness of an
    // average-weight edge) instead of being compressed into a fixed range.
    "  if ((G.flags & FLAG_EDGE_WEIGHT_WIDTH) != 0u) {\n"
    "    halfWidthPx = halfWidthPx * (edgeWeights[iid] / max(G.edgeParams.w, 1e-6));\n"
    "  }\n"
    // Stop the shaft where the arrowhead begins (past node b's own radius
    // when directed) so the two never overlap and double-blend under alpha.
    "  var screenEndB = screenB;\n"
    "  if (isDirected) { screenEndB = arrowBase; }\n"
    // Half-pixel feather margin.
    "  let quadHalf = halfWidthPx + 0.5;\n"
    "  let screen = mix(screenA, screenEndB, end) + normal * side * quadHalf;\n"
    "  let clip = vec4f(screen * 2.0 / G.viewport * clipEnd.w, clipEnd.zw);\n"
    "  let along = mix(0.0, len, end);\n"
    "\n"
    "  return EdgeOut(clip, side * quadHalf, color, halfWidthPx, along, dashed);\n"
    "}\n"
    "\n"
    // Dash period/duty cycle are fixed rather than configurable -- this is
    // meant for a small number of app-marked "not a real edge" connectors
    // (see grRendererSetEdgeDashed), not a general per-edge style knob.
    "const DASH_PERIOD_PX : f32 = 16.0;\n"
    "const DASH_DUTY      : f32 = 0.55;\n"
    "\n"
    "@fragment\n"
    "fn fsEdge(in : EdgeOut) -> @location(0) vec4f {\n"
    "  if (in.dashed != 0u && fract(in.along / DASH_PERIOD_PX) > DASH_DUTY) {\n"
    "    discard;\n"
    "  }\n"
    "  let coverage =\n"
    "      1.0 - smoothstep(in.halfWidthPx - 0.5, in.halfWidthPx + 0.5,\n"
    "                       abs(in.across));\n"
    "  if (coverage <= 0.0) { discard; }\n"
    "  return vec4f(in.color.rgb, in.color.a * coverage);\n"
    "}\n"
    ;

/**
 * WGSL for the object overlay: a small self-contained panel (background +
 * border, then a triangle mesh lit with a fixed camera-relative light) drawn
 * into its own render pass with the viewport/scissor restricted to the panel
 * rect. Layout must match grObjOverlayUBO and the bind group in
 * grObjOverlay.c.
 */
static const char GR_WGSL_OBJ_SOURCE[] =
    "struct ObjGlobals {\n"
    "  viewProj    : mat4x4f,\n"
    "  lightDir    : vec4f,\n"
    "  baseColor   : vec4f,\n"
    "  panelSizePx : vec4f,\n" // xy used
    "  texFlags    : vec4f,\n" // x: 1.0 when a texture map is active
    "}\n"
    "\n"
    "@group(0) @binding(0) var<uniform> OG : ObjGlobals;\n"
    "@group(0) @binding(1) var<storage, read> objPositions : array<f32>;\n"
    "@group(0) @binding(2) var<storage, read> objNormals   : array<f32>;\n"
    "@group(0) @binding(3) var<storage, read> objIndices   : array<u32>;\n"
    "@group(0) @binding(4) var<storage, read> objUV    : array<f32>;\n"
    "@group(0) @binding(5) var texSampler : sampler;\n"
    "@group(0) @binding(6) var tex        : texture_2d<f32>;\n"
    "\n"
    "const OBJ_CORNERS = array<vec2f, 6>(\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));\n"
    "\n"
    // -------------------------------------------------------- panel bg --
    "struct ObjBgOut {\n"
    "  @builtin(position) clip : vec4f,\n"
    "  @location(0) uv : vec2f,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsObjBg(@builtin(vertex_index) vid : u32) -> ObjBgOut {\n"
    "  let corner = OBJ_CORNERS[vid];\n"
    "  return ObjBgOut(vec4f(corner, 0.999, 1.0), corner * 0.5 + 0.5);\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fsObjBg(in : ObjBgOut) -> @location(0) vec4f {\n"
    "  let edgePx = min(in.uv, vec2f(1.0) - in.uv) * OG.panelSizePx.xy;\n"
    "  let dist = min(edgePx.x, edgePx.y);\n"
    "  let border = 1.0 - smoothstep(1.5, 2.5, dist);\n"
    "  let bg = vec4f(0.06, 0.07, 0.09, 0.88);\n"
    "  let borderColor = vec4f(1.0, 1.0, 1.0, 0.22);\n"
    "  return mix(bg, borderColor, border);\n"
    "}\n"
    "\n"
    // ---------------------------------------------------------------- mesh --
    "struct ObjOut {\n"
    "  @builtin(position) clip : vec4f,\n"
    "  @location(0) normal : vec3f,\n"
    "  @location(1) uv : vec2f,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsObj(@builtin(vertex_index) vid : u32) -> ObjOut {\n"
    "  let idx = objIndices[vid];\n"
    "  let base = idx * 3u;\n"
    "  let pos = vec3f(objPositions[base], objPositions[base + 1u],\n"
    "                  objPositions[base + 2u]);\n"
    "  let nrm = vec3f(objNormals[base], objNormals[base + 1u],\n"
    "                  objNormals[base + 2u]);\n"
    "  let uv = vec2f(objUV[idx * 2u], objUV[idx * 2u + 1u]);\n"
    "  let clip = OG.viewProj * vec4f(pos, 1.0);\n"
    "  return ObjOut(clip, nrm, uv);\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fsObj(in : ObjOut) -> @location(0) vec4f {\n"
    "  let n = normalize(in.normal);\n"
    "  let l = normalize(OG.lightDir.xyz);\n"
    "  let diffuse = max(dot(n, l), 0.0);\n"
    "  let shade = clamp(0.28 + diffuse * 0.85, 0.0, 1.0);\n"
    "  if (OG.texFlags.x > 0.5) {\n"
    "    let outsideImage = in.uv.x < 0.0 || in.uv.x > 1.0 ||\n"
    "                       in.uv.y < 0.0 || in.uv.y > 1.0;\n"
    "    if (outsideImage) {\n"
    "      return vec4f(1.0, 1.0, 1.0, OG.baseColor.a);\n"
    "    }\n"
    "    let texColor = textureSample(tex, texSampler, in.uv).rgb;\n"
    "    return vec4f(texColor * shade, OG.baseColor.a);\n"
    "  }\n"
    "  return vec4f(OG.baseColor.rgb * shade, OG.baseColor.a);\n"
    "}\n";

/**
 * WGSL for a single textured quad drawn directly in the main scene at the
 * texture map's movable image rectangle (in the same embedding-space
 * coordinates as the attached graph), so a user can see exactly how the
 * image lines up with the live graph. Drawn as 6 vertex-pulled vertices (2
 * triangles), no vertex buffers. Layout must match grTexMapImageUBO and the
 * bind group in grTextureMap.c.
 */
static const char GR_WGSL_TEXMAP_IMAGE_SOURCE[] =
    "struct ImgGlobals {\n"
    "  viewProj      : mat4x4f,\n"
    "  rectCenter    : vec2f,\n"
    "  rectHalfExtent : vec2f,\n"
    "  opacity       : f32,\n"
    "  pad0 : f32,\n"
    "  pad1 : f32,\n"
    "  pad2 : f32,\n"
    "}\n"
    "\n"
    "@group(0) @binding(0) var<uniform> IG : ImgGlobals;\n"
    "@group(0) @binding(1) var imgSampler : sampler;\n"
    "@group(0) @binding(2) var imgTex     : texture_2d<f32>;\n"
    "\n"
    "const IMG_CORNERS = array<vec2f, 6>(\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),\n"
    "  vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));\n"
    "\n"
    "struct ImgOut {\n"
    "  @builtin(position) clip : vec4f,\n"
    "  @location(0) uv : vec2f,\n"
    "}\n"
    "\n"
    "@vertex\n"
    "fn vsTexMapImage(@builtin(vertex_index) vid : u32) -> ImgOut {\n"
    "  let corner = IMG_CORNERS[vid];\n"
    "  let world = IG.rectCenter + corner * IG.rectHalfExtent;\n"
    "  let clip = IG.viewProj * vec4f(world, 0.0, 1.0);\n"
    // Same (u, v) convention as grTextureMapComputeUV: v flipped so row 0 of
    // the source image (top) lands at the rect's max-y edge.
    "  let uv = vec2f(corner.x * 0.5 + 0.5, 1.0 - (corner.y * 0.5 + 0.5));\n"
    "  return ImgOut(clip, uv);\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fsTexMapImage(in : ImgOut) -> @location(0) vec4f {\n"
    "  let c = textureSample(imgTex, imgSampler, in.uv).rgb;\n"
    "  return vec4f(c, IG.opacity);\n"
    "}\n";

#endif
