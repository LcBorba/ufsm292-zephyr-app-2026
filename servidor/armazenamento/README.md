# Servidor: item 2, leituras em CSV no cartão SD

## Objetivo

Testar, de forma isolada, a gravação das leituras dos nós sensores no cartão
microSD do IO1 Xplained Pro, em formato CSV e com timestamp.

O teste usa **dados fictícios**, então não depende de Ethernet, do gateway nem
das outras partes do servidor. Depois de validado, o código é integrado ao
servidor completo.

## Exemplo do arquivo gerado

```csv
recebido_ms,node_id,light,temp_c,accel_x,accel_y,accel_z,last_seen_ms
15230,1,320,24.53,0.010,-0.020,9.800,1234
```

## Como compilar

```bash
west build -b samd21_xpro servidor/armazenamento -p
```
