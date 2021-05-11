NOMBRE: Arnau González Vilar
NIA: 218172
MAIL: arnau.gonzalez02@estudiant.upf.edu

DESCRIPCIÓN DE OPCIONES:
La aplicación permite los siguientes modos de render seleccionables en el ImGui:

	-DEFAULT (COMPLETO CON LUCES)
	-SOLO LAS TEXTURAS DE COLOR
	-MUESTRA DE LOS UVS
	-MUESTRA DE LAS NORMALES (USANDO NORMALMAPS)
	-MUESTA DE LA TEXTURA DE OCLUSIÓN
	-MUESTRA DE LA TEXTURA EMISIVA

La aplicación permite dos modos de iluminación: mutipass (MULTI) y singlepass (SINGLE).

También permite usar un algoritmo de PCF para suavizar las sombras (sólo disponible en MULTI).

Se permite visualizar el depth buffer de las cámaras de las diferentes luces. 

Las anteriores opciones estan indicadas para ser activadas y desactivadas en el ImGui, pero para cambiar entre las diferentes luces en el depth viewport, pulsa 1.

Además se ha incluido un modo de calidad que determina (por el momento) la resolución de los depth buffers en los shadowmaps. Se escoge en el ImGui y por defecto viene en MEDIUM, dónde las opciones son:
	
	-LOW: 1024x1024
	-MEDIUM: 2048x2048
	-HIGH: 4096x4096
	-ULTRA: 8192x8192

Todos los parámetros que se indican en el shadowmap se pueden cambiar libremente con ImGui. 

Además, añadir que al usar PCF lo más probable es que haya que aumentarlo respecto los valores que vienen indicados en scene.json para obtener un resultado satisfactorio.

Por último, cabe destacar que la cámara de la luz direccional se mueve junto a la cámara desde la que visualizamos la escena, con lo que el shadowmap irá definiéndose en base a ello. Por tanto, para tener un resultado plenamente satisfactorio en ello, se recomienda aumentar la max_distance hasta unos 3000 para no tener ningún tipo de problema en la visualización con respecto a calidades LOW y MEDIUM.