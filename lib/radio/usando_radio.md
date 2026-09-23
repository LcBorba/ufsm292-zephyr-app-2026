# Usando o radio

O que `struct sensor_frame_cfg` representa, e o que passar para
`sensor_frame_encode()`.

O codec vive em `lib/radio/sensor_frame.c` e só foca em transformar os dados de um struct para o buffer e de volta.

---

## 1. Constantes de comprimento

From `include/app/lib/sensor_frame.h`:

| Constant | Value | Meaning |
|---|---|---|
| `SENSOR_PAYLOAD_LEN` | 18 | Pacote do sensor -- fixado pelo especifação do sensor |
| `SENSOR_FRAME_HEADER_LEN` | 9 | v1 MAC header: FCF(2) + seq(1) + dst PAN(2) + dst addr(2) + src addr(2) |
| `SENSOR_FRAME_LEN` | 27 | `HEADER + PAYLOAD` — comprimento do payload + header |

**Sequencia de checagem de frame (FCS) de 2-bytes não aparece** É adicionado para o frame e removido quando recebido pelo hardware (de acordo com a configuração em Kconfig), não aparece no buffer.

---

## 2. `struct sensor_frame_cfg` — campo a campo

```c
struct sensor_frame_cfg {
        uint16_t pan_id;
        uint16_t dst_short_addr;
        uint16_t src_short_addr;
        uint8_t  mac_seq;
        bool     ack_request;
};
```

### `uint16_t pan_id` — ID PAN do destino

Endereço do receptor -- deve ser definido um para o projeto inteiro. (ou o transmissor e o receptor não conseguem falar)


### `uint16_t dst_short_addr` — destination short address

Um endereço mais curto do receptor, use 0xFFFF para um broadcast

### `uint16_t src_short_addr` — source short address
Endereço curto da fonte -- deve ser igual ao id do nó


### `uint8_t mac_seq` — 802.15.4 MAC sequence number

Número de sequencia do pacote.
Deve ser incrementado manualmente quando enviando -- caso contrário o recebedor pode rejeitar o pacote.

```c
cfg.mac_seq++;          /* uint8_t, wraps naturally */
```


### `bool ack_request` — pede um ACK

Deve ser falso quando enviando para 0xFFFF.

---

## 3. O que não pode ser configurado

Já configurado pelo rádio.

```
FCF = 0x9841  (0x9861 when ack_request is true)     little-endian on the wire
```

| Bits | Field | Value |
|---|---|---|
| 0–2 | Frame type | `1` = Data |
| 3 | Security Enabled | `0` - inseguro|
| 4 | Frame Pending | `0` |
| 5 | ACK Request | 0 conforme o anterior |
| 6 | PAN ID Compression | `1` - Fonte omitida |
| 7–9 | Reserved (2006) | `0` |
| 10–11 | Dest addressing mode | `2` = Short (16-bit) |
| 12–13 | Frame version | `1` = 802.15.4-2006 |
| 14–15 | Source addressing mode | `2` = Short (16-bit) |

Algumas notas sobre estes parâmetros.

- **Versão de frame fixa em 2006.** Enviando com a revisão de 2015 causa erros.
- **Endereços de 16-bit.** Os endereços de 64 podem ser aceitos pelo receptor mas não devem ser transmitidos.
- **Sem segurança.** Rejeitados quando recebidos e mudam o comprimento do payload.

---

## 4. `*buf` and `cap`

```c
int sensor_frame_encode(uint8_t *buf, size_t cap,
                        const struct sensor_frame_cfg *cfg,
                        const struct sensor_reading *reading);
```

### `*buf` — O buffer do packet

Um buffer escrevível de >= 27 bytes.
Recebe o header seguido do payload.
Não reserve espaço para o FCD

Escrito byte por byte, respeitando o alinhamento nescessário para o Cortex M0+

Quando escrito: buf[0] .. buf[26] sobreescritos.
Quando falha nada aconteçe -- argumentos são testados antes de escrever.


### `cap` — capacidade do buffer em bytes

Passe `sizeof(buf_array)`:

```c
uint8_t psdu[SENSOR_FRAME_LEN];
ret = sensor_frame_encode(psdu, sizeof(psdu), &cfg, &r);
```

Deve ser >= 27 bytes para a escrita acontencer

### valores de retorn

| Return | Meaning |
|---|---|
| `>= 0` | Bytes escritos. sempre `SENSOR_FRAME_LEN` (27) quando tem sucesso. |
| `-EINVAL` | `buf`, `cfg` ou `reading` foi `NULL`. |
| `-ENOSPC` | `cap < SENSOR_FRAME_LEN`; nada escrito. |

É atomico -- ou escreve tudo ou falha.

---

## 5. Byte layout — the wire contract

Tudo é little endian. (no SAM e no ar)

| Offset | Len | Field | Source |
|---:|---:|---|---|
| 0 | 2 | Frame Control Field | fixed `0x9841` (+`0x0020` if `ack_request`) |
| 2 | 1 | MAC sequence number | `cfg->mac_seq` |
| 3 | 2 | Destination PAN ID | `cfg->pan_id` |
| 5 | 2 | Destination short address | `cfg->dst_short_addr` |
| 7 | 2 | Source short address | `cfg->src_short_addr` |
| 9 | 1 | `node_id` | `reading->node_id` |
| 10 | 2 | `seq` | `reading->seq` |
| 12 | 2 | `light` | `reading->light` |
| 14 | 2 | `temp_c_x100` | `reading->temp_c_x100` (signed) |
| 16 | 2 | `accel[0]` | `reading->accel[0]` (signed) |
| 18 | 2 | `accel[1]` | `reading->accel[1]` (signed) |
| 20 | 2 | `accel[2]` | `reading->accel[2]` (signed) |
| 22 | 4 | `uptime_ms` | `reading->uptime_ms` |
| 26 | 1 | `flags` | `reading->flags` |
| *27* | *2* | *FCS* | *appended by the radio hardware - never in `buf`* |

Bytes 0–8 são cabeçalho (`SENSOR_FRAME_HEADER_LEN`), bytes 9–26
os dados (`SENSOR_PAYLOAD_LEN`).

Campo são signed: `temp_c_x100` é Celsius × 100
(so 2399 means 23.99 °C), `accel[]` nas unidados do sensor.

---

## 6. Ligando o rádio

Configurar o hardware é responsabilidade do projeto, o codec só transmuta sensor_frames em buffer e não configura o hardware.
pode seguir o que `gateway/src/main.c` faz:

Canal e PAN ID combinados para o projeto: canal 15, PAN ID 0xCAFE.

```c
static const struct device *const radio =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));

api = (const struct ieee802154_radio_api *)radio->api;
api->set_channel(radio, 15);     /* 11..26; must match the receiver */
api->set_txpower(radio, 0);      /* dBm */
api->start(radio);               /* put the transceiver in RX/ready state */
```

Insira o pdsu de 27 bytes  em um `net_pkt` and chame
`api->tx(radio, IEEE802154_TX_MODE_CSMA_CA, pkt, frag)`.

Use algo da forma:

```c
/* 1. Crie pacote
*/
struct net_pkt *pkt = net_pkt_alloc_with_buffer(NULL, SENSOR_FRAME_LEN,
                                                NET_AF_UNSPEC, 0, K_NO_WAIT);
if (pkt == NULL) {
        return -ENOMEM;
}

/* 2. insira PDSU no pacote
*/
if (net_pkt_write(pkt, psdu, SENSOR_FRAME_LEN) != 0) {
        net_pkt_unref(pkt);
        return -ENOMEM;
}

/* 3. Entregue pacote para driver
 *    o rf2xx API precisa (pkt, frag) separately: frag is the actual bytes. */
struct net_buf *frag = net_buf_frag_last(pkt->buffer);
ret = api->tx(radio, IEEE802154_TX_MODE_CSMA_CA, pkt, frag);

/* 4. SEMPRE unref após mandar: o driver copia o que precisa */
net_pkt_unref(pkt);

/* 5. Incremente o numero de sequencia dos pacotes */
cfg.mac_seq++;
```

Canal e potencia devem estar em acordo entre o transmissor e o receptor (potencia é definida pelo transmissor). `cfg.pan_id` and `cfg.src_short_addr` tem que estar em acordo também. 

Se não está recebendo cheque os pares: canall, ID PAN

## 7. O que o decodificador espera

`sensor_frame_decode()` / `sensor_frame_decode_meta()` recebe os 27 bytes do rádio e transforma em um sensor_reading.

```c
struct sensor_reading r;
if (sensor_frame_decode(psd, len, &r) == 0) { /* use r */ }
```

`psd` precisa ter o FSC removido para `len` == 27
No rf2xx driver tem uma configuração para fazer isso automaticamente:

```ini
# required in every raw-mode image's prj.conf
CONFIG_IEEE802154_L2_PKT_INCL_FCS=n
```

`CONFIG_IEEE802154_RAW_MODE` faz `IEEE802154_L2_PKT_INCL_FCS` ter o padrão `y`,
o que faz o driver passar os 2 bytes extra do FCS para o decodifidor e daí a coisa falha.

`sensor_frame_parse_header()` classifica um frame rejeitado pelo decodificador: não tem requiremento dos 18 bytes de payload. 
---

## 8. Onde está implementado

Caso não bata leia as fontes:

- `tests/lib/radio/` — 23 casos que cobrem encode, decode, os cabeçalhos, encodificando e decodificando e valor que podem causar problemas..
- Pode testar no host (`west twister -T tests/lib/radio --tag unit`) e no SAM (`west build -b samr21_xpro tests/lib/radio`), mas note que o host tolera accessos não alinhados que o Cortex M0+ do SAM não tolera
- `lib/radio/radio_test_pattern.c` uma sequencia determinista de teste;
  `include/app/lib/radio_test_pattern.h` documenta a fórmula, coberta pelos testes de unidade.

Se o formato mudar por alguma razão, os vetores de teste que os valores são comparados contra precisam ser alterados.
