# Servidor (Grupo C): datalogger principal do sistema

Subsistema **C** do projeto final *Sistema de Sensoriamento Sem Fio*
(UFSM00292, Projeto de Sistemas Embarcados, 2026/2).

O servidor é o "caderno de anotações" do sistema: pergunta periodicamente ao
gateway quais são as últimas leituras dos nós sensores, grava tudo no cartão
SD e responde ao supervisor quando ele pede os dados ou o histórico.

## Equipe

| Item | Responsabilidade | Responsável |
|---|---|---|
| 1 | Rede Ethernet + consultar o gateway (`GET /sensors`) | Willian |
| 2 | Persistir as leituras no SD em CSV, com timestamp | Thaja |
| 3 | Servidor HTTP para o supervisor (`/health`, `/data`, `/log`) | Gabriel |
| 4 | Responder ao `POST /reset` (recuperação de falhas) | *a definir* |

## Onde o servidor entra no sistema

```
Nó sensor (A) ──rádio──► Gateway (B) ──Ethernet──► SERVIDOR (C) ◄──Ethernet── Supervisor (D)
mede luz, temp,          guarda a última           consulta, grava             monitora e
aceleração               leitura de cada nó        no SD e responde            recupera falhas
```

## Hardware

| Placa | Conector | Uso |
|---|---|---|
| SAMD21 Xplained Pro (ATSAMD21J18A, Cortex-M0+, 256 KB Flash, 32 KB RAM) | n/a | placa principal |
| Ethernet1 Xplained Pro (ENC28J60 via SPI) | EXT1 | rede (itens 1, 3 e 4) |
| IO1 Xplained Pro (microSD via SPI) | EXT2 | cartão SD (item 2) |

## Como as partes conversam

```
                      DENTRO DA PLACA DO SERVIDOR
          ┌──────────────────────────────────────────────────────┐
 Gateway  │  ① COLETOR                                           │
 ◄────────┼── GET /sensors a cada N s                            │
 ────────►│   JSON → struct leitura                              │
   JSON   │        │                                             │
          │        ├─────────────────────┐                       │
          │        ▼                     ▼                       │
          │  ② ARMAZENAMENTO       tabela "últimas leituras"     │
          │   1 linha no CSV do SD       │ (memória)             │
          │        │                     │                       │
          │        │ histórico           │ última de cada nó     │
          │        ▼                     ▼                       │
          │  ③ SERVIDOR HTTP  (/health, /data, /log)             │
          │  ④ POST /reset                                       │
          └────────────┬─────────────────────────────────────────┘
                       │ JSON
                       ▼
                 Supervisor (D)
```

1. **O coletor busca.** Faz `GET /sensors` no gateway e converte o JSON em
   `struct leitura`.
2. **O armazenamento guarda.** Cada leitura vira uma linha no `LEITURAS.CSV`
   do cartão SD, com o timestamp de quando o servidor a recebeu.
3. **O HTTP responde.** O `/data` usa a última leitura de cada nó (em
   memória), e o `/log` pede ao armazenamento o histórico lido do SD.

O que liga as três partes é a **`struct leitura`**: o item 1 produz, o item 2
grava e o item 3 lê. Cada parte roda na sua própria thread (requisito de
sistema multitarefas com Zephyr).

## Organização das pastas (proposta)

*Ainda a decidir pelo grupo.*

```
servidor/
├── README.md
├── coletor/          ← item 1: busca os dados no gateway
├── armazenamento/    ← item 2: grava o CSV no SD
├── api_http/         ← item 3: responde o supervisor
└── reset/            ← item 4 (ou dentro de api_http/)
```

## Documentação de cada parte

- [coletor/README.md](coletor/README.md): item 1, consulta ao gateway
- [armazenamento/README.md](armazenamento/README.md): item 2, leituras em CSV no SD
- [api_http/README.md](api_http/README.md): item 3, API HTTP para o supervisor
