import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const readRepoText = (...segments) =>
  fs.readFileSync(path.join(repoRoot, ...segments), "utf8").replace(/\r\n/g, "\n");

const contract = JSON.parse(fs.readFileSync(path.join(repoRoot, "webui/shared/ws63-api.json"), "utf8"));
const firmwareSource = readRepoText("xc/ws63_team_network/src/ws63_team_network_app.c");
const firmwareCmake = readRepoText("xc/ws63_team_network/CMakeLists.txt");
const firmwareHttpHeader = fs.existsSync(path.join(repoRoot, "xc/ws63_team_network/src/ws63_team_http.h"))
  ? readRepoText("xc/ws63_team_network/src/ws63_team_http.h")
  : "";
const firmwareHttpSource = fs.existsSync(path.join(repoRoot, "xc/ws63_team_network/src/ws63_team_http.c"))
  ? readRepoText("xc/ws63_team_network/src/ws63_team_http.c")
  : "";
const firmwareGpsSource = fs.existsSync(path.join(repoRoot, "xc/ws63_team_network/src/ws63_team_gps.c"))
  ? readRepoText("xc/ws63_team_network/src/ws63_team_gps.c")
  : "";
const cliSource = readRepoText("src/sle_team_cli.c");
const nodeHeader = readRepoText("include/sle_team_node.h");
const nodeSource = readRepoText("src/sle_team_node.c");
const webApiHeader = readRepoText("include/sle_team_web_api.h");
const webApiSource = readRepoText("src/sle_team_web_api.c");
const clientSource = readRepoText("webui/src/api/client.ts");
const typesSource = readRepoText("webui/src/protocol/types.ts");
const mainSource = readRepoText("webui/src/main.ts");
const stylesSource = readRepoText("webui/src/styles/app.css");

const activeSources = {
  "ws63_team_network_app.c": firmwareSource,
  "sle_team_node.h": nodeHeader,
  "sle_team_node.c": nodeSource,
  "sle_team_web_api.h": webApiHeader,
  "sle_team_web_api.c": webApiSource,
  "webui/src/protocol/types.ts": typesSource,
  "webui/src/api/client.ts": clientSource,
};

test("WS63 API contract lists the current board HTTP routes", () => {
  assert.deepEqual(
    contract.routes.map((route) => route.path),
    [
      "/api/status",
      "/api/nodes",
      "/api/events",
      "/api/pending",
      "/api/location",
      "/api/gps",
      "/api/config/status",
      "/api/config/leader",
      "/api/config/member",
      "/api/config/apply",
      "/api/config/clear",
      "/api/config/reboot",
      "/api/role",
      "/api/pairing",
      "/api/member/select",
      "/api/member/leave",
      "/api/factory-reset",
    ],
  );
});

test("firmware exposes the minimal rewrite version and config surface", () => {
  assert.match(firmwareSource, /#define SLE_TEAM_FW_VERSION "v4\.5\.64-minimal"/);
  assert.match(firmwareSource, /#define SLE_TEAM_FW_COMPAT 0x0564U/);
  assert.match(firmwareSource, /team_fw_compat_from_adv_data/);
  assert.match(firmwareSource, /drop rejected hello/);
  assert.match(firmwareSource, /#define SLE_TEAM_HW_CONSTRAINTS "minimal leader\/member\/relay rewrite"/);
  assert.match(firmwareSource, /\[cfg-json\]/);
  assert.match(firmwareSource, /cfg leader now/);
  assert.match(firmwareSource, /cfg member now/);
  assert.match(firmwareSource, /cfg direct/);
  assert.match(firmwareSource, /cfg relay target/);
  assert.match(cliSource, /pairing start/);
  assert.match(cliSource, /pairing stop/);
  assert.match(firmwareSource, /runtimeDirectCap/);
  assert.match(firmwareSource, /runtimeLeaderTerm/);
  assert.match(firmwareSource, /nvLeaderTerm/);
  assert.match(firmwareSource, /runtimeRelayTarget/);
  assert.match(firmwareSource, /runtimeRelayCount/);
  assert.match(firmwareSource, /runtimeOnlineCount/);
  assert.match(firmwareSource, /runtimeParent/);
  assert.match(firmwareSource, /roleRequestPending/);
  assert.match(firmwareSource, /team_request_role_config/);
  assert.match(firmwareSource, /team_handle_role_request_once/);
  assert.match(firmwareHttpSource, /starting SLE/);
  assert.match(firmwareSource, /lastRoleRet/);
  assert.doesNotMatch(firmwareSource, /runtimeRelayBudget/);
  assert.doesNotMatch(firmwareSource, /runtimeNetworkStage/);
});

test("webui exposes node locations and phone GPS fallback", () => {
  assert.match(mainSource, /renderLocationPanel/);
  assert.match(mainSource, /renderPositionCard/);
  assert.match(mainSource, /position-card/);
  assert.match(mainSource, /share-phone-location/);
  assert.match(mainSource, /navigator\.geolocation/);
  assert.match(mainSource, /satCount: readNumber\(form, "satCount"\)/);
  assert.match(clientSource, /api\/location/);
  assert.match(clientSource, /lat: command\.latitudeE6/);
  assert.match(clientSource, /lon: command\.longitudeE6/);
  assert.match(clientSource, /sat: command\.satCount/);
  assert.match(typesSource, /satCount\?: number/);
});

test("webui renders live topology and polls board APIs frequently", () => {
  assert.match(mainSource, /renderTopologyPanel/);
  assert.match(mainSource, /onlineNodeCount/);
  assert.match(mainSource, /relayNodeCount/);
  assert.match(mainSource, /topologyLeaderLabel/);
  assert.match(mainSource, /updates every 2s/);
  assert.match(mainSource, /window\.setInterval[\s\S]*}, 2000\)/);
  assert.match(mainSource, /window\.setInterval[\s\S]*}, 5000\)/);
  assert.match(stylesSource, /\.topology-graph/);
  assert.match(stylesSource, /\.topology-node\.relay/);
  assert.match(typesSource, /self\?: boolean/);
  assert.match(typesSource, /onlineNodeCount\?: number/);
  assert.match(typesSource, /relayNodeCount\?: number/);
});

test("active firmware implements the board location HTTP route", () => {
  assert.match(firmwareCmake, /ws63_team_http\.c/);
  assert.match(firmwareHttpHeader, /ws63_team_http_start/);
  assert.match(firmwareSource, /ws63_team_http_start\(&g_team_node/);
  assert.match(firmwareHttpSource, /\/api\/location/);
  assert.match(firmwareHttpSource, /team_http_handle_location/);
  assert.match(firmwareHttpSource, /team_http_send_gps_json_response/);
  assert.match(firmwareHttpSource, /team_http_write_gps_json/);
  assert.match(firmwareHttpSource, /ws63_team_gps_get_status/);
  assert.match(firmwareHttpSource, /ws63_team_gps_set_fallback_position/);
  assert.doesNotMatch(firmwareHttpSource, /role_not_ready/);
  assert.match(firmwareCmake, /sle_team_location\.c/);
  assert.match(nodeHeader, /sle_team_node_record_local_position/);
  assert.match(firmwareHttpSource, /sle_team_node_record_local_position/);
  assert.match(firmwareHttpSource, /sle_team_node_send_position/);
  assert.match(firmwareHttpSource, /sle_team_web_write_status_json/);
  assert.match(firmwareHttpSource, /sle_team_web_write_nodes_json/);
  assert.match(firmwareGpsSource, /sle_team_node_record_local_position\(node, &g_gps\.last_pos\)/);
  assert.match(firmwareGpsSource, /valid_sentences/);
  assert.match(firmwareGpsSource, /fix_sentences/);
  assert.match(firmwareGpsSource, /no_fix_sentences/);
  assert.match(firmwareGpsSource, /format_errors/);
  assert.match(webApiSource, /const sle_team_member_record_t \*self = sle_team_node_find_member/);
  assert.match(webApiSource, /\\"self\\":true/);
  assert.match(webApiSource, /\\"positionValid\\":%s/);
  assert.doesNotMatch(webApiSource, /"positionValid":false,\\"latitudeE6\\":0/);
  assert.match(firmwareHttpSource, /team_http_make_self_member_record/);
  assert.match(firmwareHttpSource, /team_http_append_node_rows/);
  assert.match(firmwareHttpSource, /g_team_http\.node->cfg\.role == SLE_TEAM_ROLE_MEMBER/);
});

test("board HTTP and firmware event surfaces expose live topology state", () => {
  assert.match(firmwareHttpSource, /TEAM_HTTP_WIFI_SECURITY_PRIMARY/);
  assert.match(firmwareHttpSource, /TEAM_HTTP_WIFI_PROTOCOL_PRIMARY/);
  assert.match(firmwareHttpSource, /conf\.security_type = TEAM_HTTP_WIFI_SECURITY_PRIMARY/);
  assert.match(firmwareHttpSource, /adv\.protocol_mode = TEAM_HTTP_WIFI_PROTOCOL_PRIMARY/);
  assert.match(firmwareHttpSource, /fallback to mix\/ax/);
  assert.match(firmwareHttpSource, /team_http_append_topology/);
  assert.match(firmwareHttpSource, /\|- relay/);
  assert.match(firmwareHttpSource, /RELAY/);
  assert.match(firmwareHttpSource, /\|-- child/);
  assert.match(firmwareHttpSource, /top-child/);
  assert.match(firmwareHttpSource, /Online Nodes/);
  assert.match(firmwareHttpSource, /Relay Nodes/);
  assert.match(firmwareHttpSource, /onlineNodeCount/);
  assert.match(firmwareHttpSource, /relayNodeCount/);
  assert.match(firmwareSource, /team_web_event\(SLE_TEAM_WEB_EVENT_SYSTEM/);
  assert.match(firmwareSource, /team_web_event\(SLE_TEAM_WEB_EVENT_RX/);
  assert.match(firmwareSource, /member joined/);
  assert.match(firmwareSource, /relay offline/);
  assert.match(firmwareSource, /member hello/);
  assert.match(webApiSource, /\\"self\\":true/);
  assert.match(webApiSource, /onlineNodeCount/);
  assert.match(webApiSource, /relayNodeCount/);
});

test("minimal node state names replace the old staged discovery contract", () => {
  assert.match(nodeHeader, /SLE_TEAM_NET_WAIT_POLICY = 1/);
  assert.match(nodeHeader, /SLE_TEAM_PARENT_WAIT_POLICY = 1/);
  assert.match(nodeSource, /SLE_TEAM_NET_WAIT_POLICY/);
  assert.match(nodeSource, /SLE_TEAM_PARENT_WAIT_POLICY/);
  assert.match(webApiSource, /return "wait_policy"/);
  assert.match(typesSource, /NodeState = "idle" \| "wait_policy" \| "joining" \| "online"/);
  assert.match(typesSource, /ParentState = "idle" \| "wait_policy" \| "connected"/);
  assert.match(clientSource, /state === 1 \? "wait_policy"/);
  assert.match(mainSource, /Wait Policy/);
});

test("minimal status API has no frozen topology metrics or reselect fields", () => {
  assert.doesNotMatch(webApiHeader, /sle_team_web_route_metrics_t/);
  assert.doesNotMatch(webApiSource, /routeMetrics/);
  assert.doesNotMatch(webApiSource, /upstreamParentReselectPending/);
  assert.doesNotMatch(typesSource, /RouteMetrics/);
  assert.doesNotMatch(typesSource, /routeMetrics/);
  assert.doesNotMatch(typesSource, /upstreamParentReselectPending/);
  assert.match(typesSource, /roleRequestPending/);
  assert.match(clientSource, /roleRequestPending/);
  assert.match(mainSource, /isStartingStatus/);
  assert.doesNotMatch(clientSource, /LeaderNetworkStage/);
  assert.doesNotMatch(mainSource, /renderRouteMetrics/);
  assert.match(mainSource, /renderMinimalRoute/);
});

test("nodes expose only minimal leader-assigned route state", () => {
  assert.match(webApiSource, /\\"parentId\\":%u/);
  assert.match(webApiSource, /\\"nextHopId\\":%u/);
  assert.match(webApiSource, /\\"childCount\\":%u/);
  assert.match(webApiSource, /\\"policyPending\\":%s/);
  assert.match(typesSource, /parentId\?: number/);
  assert.match(typesSource, /nextHopId\?: number/);
  assert.match(typesSource, /childCount\?: number/);
  assert.match(typesSource, /policyPending\?: boolean/);
  assert.match(clientSource, /parentMatch = line\.match/);
  assert.match(clientSource, /policyPending:/);
  assert.match(mainSource, /children \$\{node\.childCount\}/);
});

test("retired staged topology tokens are absent from active minimal sources", () => {
  const retiredTokens = [
    "TEAM_LEADER_NET_STAGE",
    "SLE_TEAM_NET_DISCOVERING",
    "SLE_TEAM_PARENT_DISCOVERING",
    "LeaderNetworkStage",
    "DISCOVERING",
    "FREEZING",
    "PROVISIONING",
    "STABLE",
    "team_leader_topology_",
    "topology_frozen",
    "topology_policy",
    "stable_rejoin",
    "route_hint",
    "routeMetrics",
    "failover",
    "try_parent_switch",
    "reselect",
  ];

  for (const [name, source] of Object.entries(activeSources)) {
    for (const token of retiredTokens) {
      assert.doesNotMatch(source, new RegExp(token), `${name} still contains retired token ${token}`);
    }
  }
});

test("vite dev:https alias remains mapped to the dev command", () => {
  const webPackage = JSON.parse(fs.readFileSync(path.join(repoRoot, "webui/package.json"), "utf8"));
  assert.equal(typeof webPackage.scripts["dev:https"], "string");
  assert.match(webPackage.scripts["dev:https"], /--host 0\.0\.0\.0/);
  assert.equal(webPackage.scripts["dev:https"], webPackage.scripts.dev);
});
