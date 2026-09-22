"""运行真实 TLS Connect，保护接收线程的内存来源与失败清理。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_tls_receiver_uses_external_stack_and_cleans_failed_creation(tmp_path):
    source = (ROOT / 'components/78__esp-ml307/src/esp/esp_ssl.cc').read_text()
    connect = source[source.index('bool EspSsl::Connect('):source.index('void EspSsl::Disconnect()')]
    program = r'''
#include <cassert>
#include <string>
#include <vector>
#include <cstddef>
using esp_err_t = int;
using esp_tls_error_handle_t = int;
struct esp_tls_t {};
struct esp_tls_cfg_t { void* crt_bundle_attach; int timeout_ms; const char* common_name; };
void* esp_crt_bundle_attach = nullptr;
constexpr int ESP_OK=0, ESP_ERR_NO_MEM=0x101, pdPASS=1;
constexpr int MALLOC_CAP_SPIRAM=4, MALLOC_CAP_8BIT=8, MALLOC_CAP_INTERNAL=16;
constexpr int ESP_SSL_EVENT_RECEIVE_TASK_EXIT=1;
int caps=0, destroyed=0; bool allocation_ok=true, fail_direct=false;
std::vector<std::string> connected_hosts, common_names;
esp_tls_t tls;
esp_tls_t* esp_tls_init(){return &tls;}
int esp_tls_conn_new_sync(const char* host,size_t length,int,esp_tls_cfg_t* cfg,esp_tls_t*){
 connected_hosts.emplace_back(host,length);
 common_names.emplace_back(cfg->common_name ? cfg->common_name : "");
 return fail_direct && connected_hosts.back()=="203.0.113.10" ? 0 : 1;
}
int esp_tls_get_error_handle(esp_tls_t*,int*){return -1;}
int esp_tls_get_and_clear_last_error(int,int*,int*){return 0;}
void esp_tls_conn_destroy(esp_tls_t*){++destroyed;}
void xEventGroupClearBits(int,int){}
void xEventGroupSetBits(int,int){}
void vTaskDelete(void*){}
void vTaskDeleteWithCaps(void*){}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,int*){caps=0;return allocation_ok?pdPASS:0;}
int xTaskCreateWithCaps(void(*)(void*),const char*,int,void*,int,int*,int c){caps=c;return allocation_ok?pdPASS:0;}
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CONFIG_YGSOUL_TLS_DIRECT_HOST "fixture.invalid"
#define CONFIG_YGSOUL_TLS_DIRECT_IP "203.0.113.10"
class EspSsl { public:
 esp_tls_t* tls_client_=nullptr; int event_group_=1, receive_task_handle_=0, last_error_=0;
 bool connected_=false, external_stack_=false;
 bool Connect(const std::string&,int); void ReceiveTask(){}
};
'''
    program += connect + r'''
int main(){
 EspSsl voice; assert(voice.Connect("fixture.invalid",443));
 assert(connected_hosts==std::vector<std::string>{"203.0.113.10"});
 assert(common_names==std::vector<std::string>{"fixture.invalid"});
 assert(caps == (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
 connected_hosts.clear(); common_names.clear(); fail_direct=true;
 EspSsl fallback; assert(fallback.Connect("fixture.invalid",443));
 assert((connected_hosts==std::vector<std::string>{"203.0.113.10","fixture.invalid"}));
 assert((common_names==std::vector<std::string>{"fixture.invalid",""}));
 fail_direct=false;
 EspSsl ok; ok.external_stack_=true; assert(ok.Connect("fixture.invalid",443));
 assert(caps == (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 allocation_ok=false; destroyed=0; EspSsl fail;
 assert(!fail.Connect("fixture.invalid",443));
 assert(!fail.connected_ && fail.tls_client_==nullptr);
 assert(fail.last_error_==ESP_ERR_NO_MEM && destroyed==1);
}
'''
    path = tmp_path / 'tls.cc'
    path.write_text(program)
    binary = tmp_path / 'tls'
    subprocess.run(['c++', '-std=c++17', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    assert 'vTaskDeleteWithCaps(NULL);' in connect
    network = (ROOT / 'components/78__esp-ml307/src/esp/esp_network.cc').read_text()
    assert 'std::make_unique<EspSsl>(connect_id == 2 || connect_id == 3)' in network


def test_tls_connect_timeout_fails_fast_enough_for_pairing_retry():
    source = (ROOT / 'components/78__esp-ml307/src/esp/esp_ssl.cc').read_text(encoding='utf-8')
    connect = source[source.index('bool EspSsl::Connect('):source.index('void EspSsl::Disconnect()')]

    assert 'cfg.timeout_ms = 8000;' in connect


def test_175c_test_api_bypasses_fake_ip_dns():
    sdkconfig = (ROOT / 'sdkconfig.175c').read_text(encoding='utf-8')

    assert 'CONFIG_YGSOUL_TLS_DIRECT_HOST="yomitest.gwcz.online"' in sdkconfig
    assert 'CONFIG_YGSOUL_TLS_DIRECT_IP="118.145.230.85"' in sdkconfig


def test_tls_disconnect_wakes_blocked_receiver_before_close():
    source = (ROOT / 'components/78__esp-ml307/src/esp/esp_ssl.cc').read_text(encoding='utf-8')
    disconnect = source[source.index('void EspSsl::Disconnect()'):source.index('int EspSsl::Send(')]

    assert disconnect.index('shutdown(sockfd, SHUT_RDWR);') < disconnect.index('close(sockfd);')
