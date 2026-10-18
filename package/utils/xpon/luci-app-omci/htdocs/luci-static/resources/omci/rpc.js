'use strict';
'require baseclass';
'require rpc';

var object = 'omci.agent';

function declare(method, params, expect) {
	return rpc.declare({
		object: object,
		method: method,
		params: params || [],
		expect: expect || {}
	});
}

return baseclass.extend({
	family: declare('family', [], { family: {} }),
	status: declare('status', [ 'device' ], { status: {} }),
	liveStatus: declare('status_live', [ 'device' ], { status: {} }),
	agentSet: declare('agent_set', [ 'device', 'enabled', 'permissive', 'fake_omci' ], {}),
	configList: declare('config_list', [ 'device' ], { config: {} }),
	configDetails: declare('config_details', [ 'device' ], { config: {} }),
	configGet: declare('config_get', [ 'device', 'key' ], { config: {} }),
	configInspect: declare('config_inspect', [ 'device', 'key' ], { config: {} }),
	configSet: declare('config_set', [ 'device', 'key', 'value' ], {}),
	mibList: declare('mib_list', [ 'device' ], { objects: [] }),
	apply: declare('apply', [ 'device' ], {}),
	profileList: declare('profile_list', [], { profiles: [] }),
	profileApply: declare('profile_apply', [ 'device', 'profile', 'serial', 'password' ], {}),
	vlanStatus: rpc.declare({ object: 'omci.vlan', method: 'status', expect: { '': {} } })
});
