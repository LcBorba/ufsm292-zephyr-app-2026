#!/bin/bash
set -e

# Nesta imagem (embeddedcontainers/zephyr), west roda dentro do virtualenv
# do proprio Zephyr SDK e ja deve estar disponivel. So instala se faltar,
# sem a flag --user (que nao funciona dentro de virtualenvs).
command -v west >/dev/null 2>&1 || pip3 install west

cd /workspace

# Trata o repositório montado (que já contém west.yml na raiz) como o
# repositório-manifesto local do workspace west.
west init -l ufsm292-zephyr-app-2026

# Baixa o kernel Zephyr e os módulos (HALs, CMSIS, etc.) declarados
# no west.yml como pastas irmãs em /workspace (zephyr/, modules/, ...).
west update

# Só o "base" (inclui jsonschema, usado pelo "west zephyr-export" logo
# abaixo). O requirements.txt completo do Zephyr traz também os
# requirements de compliance/testes/extras — pacotes pesados demais
# (e um deles, tree-sitter-cmake, precisa de um compilador C que esta
# imagem enxuta não tem) e desnecessários só para compilar e gravar.
pip3 install -r zephyr/scripts/requirements-base.txt

west zephyr-export

# Conveniências de terminal
echo "alias ll='ls -lah'" >> "$HOME/.bashrc"
echo "cd /workspace/ufsm292-zephyr-app-2026" >> "$HOME/.bashrc"
west completion bash > "$HOME/west-completion.bash"
echo 'source $HOME/west-completion.bash' >> "$HOME/.bashrc"

history -c
