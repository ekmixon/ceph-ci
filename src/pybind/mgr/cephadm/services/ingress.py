import ipaddress
import logging
import random
import string
from typing import List, Dict, Any, Tuple, cast, Optional

from ceph.deployment.service_spec import IngressSpec
from cephadm.utils import resolve_ip

from cephadm.services.cephadmservice import CephadmDaemonDeploySpec, CephService

logger = logging.getLogger(__name__)


class IngressService(CephService):
    TYPE = 'ingress'

    def primary_daemon_type(self) -> str:
        return 'haproxy'

    def per_host_daemon_type(self) -> Optional[str]:
        return 'keepalived'

    def prepare_create(
            self,
            daemon_spec: CephadmDaemonDeploySpec,
    ) -> CephadmDaemonDeploySpec:
        if daemon_spec.daemon_type == 'haproxy':
            return self.haproxy_prepare_create(daemon_spec)
        if daemon_spec.daemon_type == 'keepalived':
            return self.keepalived_prepare_create(daemon_spec)
        assert False, "unexpected daemon type"

    def generate_config(
            self,
            daemon_spec: CephadmDaemonDeploySpec
    ) -> Tuple[Dict[str, Any], List[str]]:
        if daemon_spec.daemon_type == 'haproxy':
            return self.haproxy_generate_config(daemon_spec)
        else:
            return self.keepalived_generate_config(daemon_spec)
        assert False, "unexpected daemon type"

    def haproxy_prepare_create(
            self,
            daemon_spec: CephadmDaemonDeploySpec,
    ) -> CephadmDaemonDeploySpec:
        assert daemon_spec.daemon_type == 'haproxy'

        daemon_id = daemon_spec.daemon_id
        host = daemon_spec.host
        spec = cast(IngressSpec, self.mgr.spec_store[daemon_spec.service_name].spec)

        logger.debug('prepare_create haproxy.%s on host %s with spec %s' % (
            daemon_id, host, spec))

        daemon_spec.final_config, daemon_spec.deps = self.haproxy_generate_config(daemon_spec)

        return daemon_spec

    def haproxy_generate_config(
            self,
            daemon_spec: CephadmDaemonDeploySpec,
    ) -> Tuple[Dict[str, Any], List[str]]:
        spec = cast(IngressSpec, self.mgr.spec_store[daemon_spec.service_name].spec)
        assert spec.backend_service
        daemons = self.mgr.cache.get_daemons_by_service(spec.backend_service)
        deps = [d.name() for d in daemons]

        # generate password?
        pw_key = f'{spec.service_name()}/monitor_password'
        password = self.mgr.get_store(pw_key)
        if password is None:
            if not spec.monitor_password:
                password = ''.join(random.choice(string.ascii_lowercase) for _ in range(20))
                self.mgr.set_store(pw_key, password)
        else:
            if spec.monitor_password:
                self.mgr.set_store(pw_key, None)
        if spec.monitor_password:
            password = spec.monitor_password

        haproxy_conf = self.mgr.template.render(
            'services/ingress/haproxy.cfg.j2',
            {
                'spec': spec,
                'servers': [
                    {
                        'name': d.name(),
                        'ip': d.ip or resolve_ip(self.mgr.inventory.get_addr(str(d.hostname))),
                        'port': d.ports[0],
                    } for d in daemons if d.ports
                ],
                'user': spec.monitor_user or 'admin',
                'password': password,
                'ip': daemon_spec.ip or '*',
                'frontend_port': daemon_spec.ports[0] if daemon_spec.ports else spec.frontend_port,
                'monitor_port': daemon_spec.ports[1] if daemon_spec.ports else spec.monitor_port,
            }
        )
        config_files = {
            'files': {
                "haproxy.cfg": haproxy_conf,
            }
        }
        if spec.ssl_cert:
            ssl_cert = spec.ssl_cert
            if isinstance(ssl_cert, list):
                ssl_cert = '\n'.join(ssl_cert)
            config_files['files']['haproxy.pem'] = ssl_cert

        return config_files, sorted(deps)

    def keepalived_prepare_create(
            self,
            daemon_spec: CephadmDaemonDeploySpec,
    ) -> CephadmDaemonDeploySpec:
        assert daemon_spec.daemon_type == 'keepalived'

        daemon_id = daemon_spec.daemon_id
        host = daemon_spec.host
        spec = cast(IngressSpec, self.mgr.spec_store[daemon_spec.service_name].spec)

        logger.debug('prepare_create keepalived.%s on host %s with spec %s' % (
            daemon_id, host, spec))

        daemon_spec.final_config, daemon_spec.deps = self.keepalived_generate_config(daemon_spec)

        return daemon_spec

    def keepalived_generate_config(
            self,
            daemon_spec: CephadmDaemonDeploySpec,
    ) -> Tuple[Dict[str, Any], List[str]]:
        spec = cast(IngressSpec, self.mgr.spec_store[daemon_spec.service_name].spec)
        assert spec.backend_service

        # generate password?
        pw_key = f'{spec.service_name()}/keepalived_password'
        password = self.mgr.get_store(pw_key)
        if password is None:
            if not spec.keepalived_password:
                password = ''.join(random.choice(string.ascii_lowercase) for _ in range(20))
                self.mgr.set_store(pw_key, password)
        else:
            if spec.keepalived_password:
                self.mgr.set_store(pw_key, None)
        if spec.keepalived_password:
            password = spec.keepalived_password

        daemons = self.mgr.cache.get_daemons_by_service(spec.service_name())
        deps = sorted([d.name() for d in daemons if d.daemon_type == 'haproxy'])

        host = daemon_spec.host
        hosts = sorted(list(set([host] + [str(d.hostname) for d in daemons])))

        # interface
        bare_ip = str(spec.virtual_ip).split('/')[0]
        interface = None
        for subnet, ifaces in self.mgr.cache.networks.get(host, {}).items():
            if ifaces and ipaddress.ip_address(bare_ip) in ipaddress.ip_network(subnet):
                interface = list(ifaces.keys())[0]
                logger.info(
                    f'{bare_ip} is in {subnet} on {host} interface {interface}'
                )
                break
        if not interface and spec.networks:
            # hmm, try spec.networks
            for subnet, ifaces in self.mgr.cache.networks.get(host, {}).items():
                if subnet in spec.networks:
                    interface = list(ifaces.keys())[0]
                    logger.info(
                        f'{spec.virtual_ip} will be configured on {host} interface '
                        f'{interface} (which has guiding subnet {subnet})'
                    )
                    break
        if not interface:
            interface = 'eth0'

        # script to monitor health
        script = '/usr/bin/false'
        for d in daemons:
            if d.hostname == host:
                if d.daemon_type == 'haproxy':
                    assert d.ports
                    port = d.ports[1]   # monitoring port
                    script = f'/usr/bin/curl http://{d.ip or "localhost"}:{port}/health'
        assert script

        # set state. first host in placement is master all others backups
        state = 'BACKUP'
        if hosts[0] == host:
            state = 'MASTER'

        # remove host, daemon is being deployed on from hosts list for
        # other_ips in conf file and converter to ips
        hosts.remove(host)

        other_ips = [resolve_ip(self.mgr.inventory.get_addr(h)) for h in hosts]

        keepalived_conf = self.mgr.template.render(
            'services/ingress/keepalived.conf.j2',
            {
                'spec': spec,
                'script': script,
                'password': password,
                'interface': interface,
                'state': state,
                'other_ips': other_ips,
                'host_ip': resolve_ip(self.mgr.inventory.get_addr(host)),
            }
        )

        config_file = {
            'files': {
                "keepalived.conf": keepalived_conf,
            }
        }

        return config_file, deps
