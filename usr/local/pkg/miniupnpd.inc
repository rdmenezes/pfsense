<?php
	require_once("util.inc");
	require_once("config.inc");
	require_once("functions.inc");
	require_once("shaper.inc");

	/* MiniUPnPd */

	function upnp_notice ($msg) { log_error("miniupnpd: {$msg}"); }
	function upnp_warn ($msg) { log_error("miniupnpd: {$msg}"); }

	function upnp_running () {
		if((int)exec('/bin/pgrep -a miniupnpd | /usr/bin/wc -l') > 0)
			return true;
		return false;
	}

	function upnp_write_config($file, $text) {
		$handle = fopen($file, 'w');
		if(!$handle) {
			upnp_warn("Could not open {$file} for writing.");
			return;
		}
		fwrite($handle, $text);
		fclose($handle);
	}

	function upnp_uuid() {
		/* md5 hash of wan mac */
		$uuid = md5(get_interface_mac(get_real_interface("wan")));
		/* put uuid in correct format 8-4-4-4-12 */
		return substr($uuid,0,8).'-'.substr($uuid,9,4).'-'.substr($uuid,13,4).'-'.substr($uuid,17,4).'-'.substr($uuid,21,12);
	}

	function upnp_validate_queue($qname) {
		read_altq_config();
		$qlist = get_altq_name_list();
		if (is_array($qlist)) {
			return in_array($qname, $qlist);
		} else {
			return false;
		}
	}

	function upnp_validate_ip($ip, $check_cdir) {
		/* validate cidr */	
		$ip_array = array();
		if($check_cdir)	{
			$ip_array = explode('/', $ip);
			if(count($ip_array) == 2) {
				if($ip_array[1] < 1 || $ip_array[1] > 32)
					return false;
			} else
				if(count($ip_array) != 1)
					return false;
		} else
			$ip_array[] = $ip;

		/* validate ip */
		if (!is_ipaddr($ip_array[0]))
			return false;
		return true;
	}

	function upnp_validate_port($port) {
		foreach(explode('-', $port) as $sub)
			if($sub < 0 || $sub > 65535)
				return false;
		return true;	
	}

	function before_form_miniupnpd($pkg) {
		global $config;

	}

	function validate_form_miniupnpd($post, $input_errors) {
		if($post['enable'] && (!$post['enable_upnp'] && !$post['enable_natpmp']))
			$input_errors[] = 'At least one of \'UPnP\' or \'NAT-PMP\' must be allowed';
		if($post['iface_array'])
			foreach($post['iface_array'] as $iface)
				if($iface == 'wan')
					$input_errors[] = 'It is a security risk to specify WAN in the \'Interface\' field';
		if($post['overridewanip'] && !upnp_validate_ip($post['overridewanip'],false))
			$input_errors[] = 'You must specify a valid ip address in the \'Override WAN address\' field';
		if(($post['download'] && !$post['upload']) || ($post['upload'] && !$post['download']))
			$input_errors[] = 'You must fill in both \'Maximum Download Speed\' and \'Maximum Upload Speed\' fields';
		if($post['download'] && $post['download'] <= 0)
			$input_errors[] = 'You must specify a value greater than 0 in the \'Maximum Download Speed\' field';
		if($post['upload'] && $post['upload'] <= 0)
			$input_errors[] = 'You must specify a value greater than 0 in the \'Maximum Upload Speed\' field';
		if($post['upnpqueue'] && !upnp_validate_queue($post['upnpqueue']))
			$input_errors[] = 'You must specify a valid traffic shaping queue.';

		/* user permissions validation */
		for($i=1; $i<=4; $i++) {
			if($post["permuser{$i}"]) {
				$perm = explode(' ',$post["permuser{$i}"]);
				/* should explode to 4 args */
				if(count($perm) != 4) {
					$input_errors[] = "You must follow the specified format in the 'User specified permissions {$i}' field";
				} else {
					/* must with allow or deny */
					if(!($perm[0] == 'allow' || $perm[0] == 'deny'))
						$input_errors[] = "You must begin with allow or deny in the 'User specified permissions {$i}' field";
					/* verify port or port range */
					if(!upnp_validate_port($perm[1]) || !upnp_validate_port($perm[3]))
						$input_errors[] = "You must specify a port or port range between 0 and 65535 in the 'User specified
							permissions {$i}' field";
					/* verify ip address */
					if(!upnp_validate_ip($perm[2],true))
						$input_errors[] = "You must specify a valid ip address in the 'User specified permissions {$i}' field";
				}
			}
		}		
	}

	function sync_package_miniupnpd() {
		global $config;
		global $input_errors;

		$upnp_config = $config['installedpackages']['miniupnpd']['config'][0];
		$config_file = '/var/etc/miniupnpd.conf';

		$config_text = "ext_ifname=".get_real_interface()."\n";
		$config_text .= "port=2189\n";

		$ifaces_active = '';

		/* since config is written before this file invoked we don't need to read post data */
		if($upnp_config['enable'] && !empty($upnp_config['iface_array'])) {
			$iface_array = explode(',', $upnp_config['iface_array']);

			foreach($iface_array as $iface) {
				$if = convert_friendly_interface_to_real_interface_name($iface);
				/* above function returns iface if fail */
				if($if!=$iface) {
					$addr = find_interface_ip($if);
					$bits = find_interface_subnet($if);
					/* check that the interface has an ip address before adding parameters */
					if (is_ipaddr($addr)) {
						$config_text .= "listening_ip={$addr}/{$bits}\n";
						if(!$ifaces_active) {
							$webgui_ip = $addr;
							$ifaces_active = $iface;
						} else
							$ifaces_active .= ", {$iface}";
					} else
						upnp_warn("Interface {$iface} has no ip address, ignoring");
				} else
					upnp_warn("Could not resolve real interface for {$iface}");
			}

			if (!empty($ifaces_active)) {
				/* override wan ip address, common for carp, etc */
				if($upnp_config['overridewanip'])
					$config_text .= "ext_ip={$upnp_config['overridewanip']}\n";

				$download = $upnp_config['download']*1000;
				$upload = $upnp_config['upload']*1000;

				/* set upload and download bitrates */
				if(!empty($download) && !empty($upload)) {
					$config_text .= "bitrate_down={$download}\n";
					$config_text .= "bitrate_up={$upload}\n";
				}
				
				/* enable logging of packets handled by miniupnpd rules */
				if($upnp_config['logpackets'])
					$config_text .= "packet_log=yes\n";
				
				/* enable system uptime instead of miniupnpd uptime */
				if($upnp_config['sysuptime'])
					$config_text .= "system_uptime=yes\n";

				/* set webgui url */
				if(!empty($config['system']['webgui']['protocol'])) {
					$config_text .= "presentation_url={$config['system']['webgui']['protocol']}://{$webgui_ip}";
					if(!empty($config['system']['webgui']['port']))
						$config_text .= ":{$config['system']['webgui']['port']}";
					$config_text .= "/\n";
				}

				/* set uuid and serial */
				$config_text .= "uuid=".upnp_uuid()."\n";
				$config_text .= "serial=".strtoupper(substr(upnp_uuid(),0,8))."\n";

				/* set model number */
				$config_text .= "model_number=".file_get_contents("/etc/version")."\n";
	
				/* upnp access restrictions */
				for($i=1; $i<=4; $i++) {
					if($upnp_config["permuser{$i}"])
						$config_text .= "{$upnp_config["permuser{$i}"]}\n";
				}

				if($upnp_config['permdefault'])
					$config_text .= "deny 0-65535 0.0.0.0/0 0-65535\n";

				/* Recheck if queue is valid */
				if (!upnp_validate_queue($upnp_config['upnpqueue']))
					unset($upnp_config['upnpqueue']);

				/* Add shaper queue */
				if($upnp_config['upnpqueue'])
					$config_text .= "queue={$upnp_config['upnpqueue']}\n";

				/* Allow UPnP or NAT-PMP as requested */
				$config_text .= "enable_upnp="   . ( $upnp_config['enable_upnp']   ? "yes\n" : "no\n" );
				$config_text .= "enable_natpmp=" . ( $upnp_config['enable_natpmp'] ? "yes\n" : "no\n" );

				/* write out the configuration */
				upnp_write_config($config_file, $config_text);
				
				/* if miniupnpd not running start it */
				if(!upnp_running()) {
					upnp_notice("Starting service on interface: {$ifaces_active}");
					upnp_action('start');	
				}
				/* or restart miniupnpd if settings were changed */
				else {
					upnp_notice("Restarting service on interface: {$ifaces_active}");
					upnp_action('restart');
				}
			}
		} else {
			/* user does not want miniupnpd running */
			/* lets stop the service and remove the rc file */

			if (file_exists($config_file)) {
				if(!$upnp_config['enable'])
					upnp_notice('Stopping service: miniupnpd disabled');
				else
					upnp_notice('Stopping service: no interfaces selected');				

				upnp_action('stop');
				@unlink($config_file);
			}
		}
	}
?>
