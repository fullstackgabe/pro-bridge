# pro-bridge

Servidor DSU (protocolo CemuHook) para macOS que lê controles com **SDL3** e os entrega a emuladores como o **Astris**.

## Por que existe

No macOS, o framework GameController da Apple recebe o **Switch Pro Controller via Bluetooth** com o analógico esquerdo quebrado: ele chega como D-pad digital (-1/0/1) e o eixo analógico nunca é reportado. O Astris lê controles por esse framework, então o personagem só anda pelo D-pad ([Astris.Binaries#281](https://github.com/V380-Ori/Astris.Binaries/issues/281)).

O SDL3 lê o controle direto por HID (o mesmo caminho do Ryujinx Canary, onde o controle funciona). O pro-bridge usa o SDL3 e repassa botões, analógicos e giroscópio por DSU, que o Astris aceita como controle completo.

## Requisitos

- macOS em Apple Silicon
- [Homebrew](https://brew.sh) com SDL3: `brew install sdl3`

## Compilar e rodar

```sh
make
./pro-bridge            # escuta em 127.0.0.1:26760
./pro-bridge --verbose  # mostra os valores dos analógicos a cada 0,5 s
```

Opções: `--host`, `--port`, `--no-motion`, `--verbose`.

Se o controle não aparecer, libere o Terminal em **Ajustes do Sistema → Privacidade e Segurança → Monitoramento de Entrada**.

## Configurar no Astris

1. Conecte o Pro Controller por Bluetooth e deixe o `pro-bridge` rodando.
2. Nas configurações de controle do Astris, escolha o controle **DSU** para o Player 1, com servidor `127.0.0.1` e porta `26760`, slot 1.
3. Não deixe o Pro Controller nativo (GameController) ligado a outro player, senão o mesmo controle entra duas vezes.

## Mapeamento

| Pro Controller | DSU |
| --- | --- |
| A / B / X / Y | Círculo / Cruz / Triângulo / Quadrado (mesmas posições) |
| L / R / ZL / ZR | L1 / R1 / L2 / R2 |
| − / + | Share / Options |
| Home | PS |
| Captura | Touchpad (clique) |
| Analógicos | 0–255, centro 128, Y positivo para cima |
| Giroscópio | aceleração em g, rotação em graus/s, eixos do SDL (x direita, y cima, z na direção do jogador) |

Até 4 controles, nos slots 1 a 4, por ordem de conexão.
