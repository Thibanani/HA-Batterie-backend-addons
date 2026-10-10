# Changelog

## 1.1.0

### Sécurité
- Clé API comparée en temps constant ; une clé vide refuse tout.
- `GET /status` exige désormais l'en-tête `X-API-Key` (`/health` reste public).
- Rapports validés strictement : types, plages physiques (tension 0-70 V, courant ±1000 A,
  SOC/SOH 0-100), `discharge_off` booléen, NaN refusé, taille du corps limitée à 4 Ko.
- Serveur HTTP durci : timeout de 15 s, 16 connexions simultanées au plus, version de
  Python masquée, logs protégés contre l'injection de caractères de contrôle.
- Interrupteur « décharge » : seules les valeurs `ON` / `OFF` sont acceptées.
- Option `api_key` masquée dans l'interface ; avertissement si la clé est une valeur
  d'exemple ou fait moins de 16 caractères.

### Fiabilité
- L'add-on démarre même si le broker MQTT est absent (y compris quand le service MQTT
  n'est pas installé) et se reconnecte automatiquement.
- Arrêt propre (SIGTERM) ; watchdog du Supervisor sur `/health`.
- Entités indisponibles si l'add-on s'arrête (LWT) ou si la passerelle ne remonte plus
  rien pendant `sensor_expire_after` secondes.

### Nouveautés
- Option `batteries` : liste des batteries (`id` envoyé par la passerelle, `name` utilisé
  dans Home Assistant) modifiable sans reconstruire l'add-on.
- Option `sensor_expire_after` (900 s par défaut, 0 = désactivé).
- Capteurs avec `state_class` (statistiques long terme), précision d'affichage et icônes.

### À vérifier après la mise à jour
- Remplacer la clé API d'exemple, dans l'add-on **et** dans la passerelle.
- Renseigner les vraies batteries dans l'option `batteries`. Changer le `name` d'une
  batterie crée de nouvelles entités dans Home Assistant.
