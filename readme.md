# Aula 04 - Nginx <a name="unifaat-frontend-project"></a>

## 📑 Sumário

- [Instalação e Execução](#instalacao-e-execucao)
- [Acesse](#acesse)
- [🚀 Como Criar Elementos](#como-criar-elementos)
  - [🧩 Criar uma Rota](#criar-uma-rota)
  - [📦 Criar um Controller](#criar-um-controller)
  - [⛓️ Criar um Middleware](#criar-um-middleware)
  - [💻 Criar um Command](#criar-um-command)
  - [📦 Migrations do Projeto](#migrations)
  - [🌱 Seeds do Projeto](#seeds)
- [📦 Bibliotecas Utilizadas](#bibliotecas-utilizadas)
- [📁 Estrutura de Diretórios (raiz)](#estrutura-de-diretorios-raiz)
- [🧾 Como Criar um Novo Documento Swagger](#swagger)
- [🐳 Containers e Imagens Docker](#containers-e-imagens-docker)

---

## Instalação e Execução <a name="instalacao-e-execucao"></a>

### Siga os passos abaixo para rodar o projeto via Docker:

1. Clonar o repositório:

   ```sh
   git clone https://github.com/luan-tavares/unifaat-frontend-project
   ```

2. Entrar na pasta do projeto:

   ```sh
   cd unifaat-frontend-project
   ```

3. Criar o arquivo `.env` na raiz do projeto copiando o `.env.example`:


   ```sh
   cp .env.example .env
   ```

4. Abrir o arquivo `.env` recém criado e preencher os campos abaixo:

   ```env
   POSTGRES_PASSWORD=123456
   JWT_SECRET=segredo
   ```

5. Instalar as dependências:

   ```sh
   npm install
   ```

6. Subir a aplicação com Docker Compose:

   > Docker Compose tradicional:

   ```sh
   docker-compose up --build
   ```

   > Docker Compose moderno:

   ```sh
   docker compose up --build
   ```

---

## Acesse <a name="acesse"></a>

- Servidor nginx: [http://localhost:8080](http://localhost:8080)

---
