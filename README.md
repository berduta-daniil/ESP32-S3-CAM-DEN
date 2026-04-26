# GitHub Pages site

Статичний сайт для перегляду відео/аудіо з плати та відправки мікрофона браузера
на динамік ESP через хмарний relay.

## Як публікувати

- або через GitHub Pages workflow з `/.github/workflows/deploy-pages.yml`
- або вручну, вказавши папку `github-pages/` як source

Після відкриття сторінки:

1. Вкажи `Relay URL`, наприклад `https://relay.example.com`
2. Вкажи `Device ID`
3. Вкажи `Viewer token`
4. Натисни `Connect`

Тепер:

- `Start A/V` вмикає відео й звук з плати
- `Start talkback` шле мікрофон телефона/ПК на динамік плати
