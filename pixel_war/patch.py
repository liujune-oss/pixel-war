import re

with open("pixel_war.ino", "r", encoding="utf-8") as f:
    text = f.read()

words = [
    "initGame", "spawnEnemy", "spawnBullet", "checkCollisions", 
    "updateEnemies", "updateBullets", "drawLEDs", "drawIdleScreen", 
    "drawGameOver", "handleButtons"
]

for w in words:
    text = re.sub(r'\b' + w + r'\b', f'lightBeam_{w}', text)

with open("pixel_war.ino", "w", encoding="utf-8") as f:
    f.write(text)
print("Finished renaming prefix.")
