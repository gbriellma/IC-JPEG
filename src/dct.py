import math
import numpy as np
from constantes import C1, S1, C3, S3, C6, S6, SQRT_2

# ----------------- Algoritmo Rápido de Loeffler -----------------
def dct_loeffler_1d(vetor_entrada):
    vetor = np.asarray(vetor_entrada, dtype=np.float32).flatten() 
    
    # Estágio 1: Borboletas de Adição/Subtração
    soma_07, dif_07 = vetor[0] + vetor[7], vetor[0] - vetor[7]
    soma_16, dif_16 = vetor[1] + vetor[6], vetor[1] - vetor[6]
    soma_25, dif_25 = vetor[2] + vetor[5], vetor[2] - vetor[5]
    soma_34, dif_34 = vetor[3] + vetor[4], vetor[3] - vetor[4]

    # Estágio 2: Reagrupamento para componentes pares
    par_0, par_3 = soma_07 + soma_34, soma_07 - soma_34
    par_1, par_2 = soma_16 + soma_25, soma_16 - soma_25

    # Estágio 3: Rotações (componentes pares)
    saida_Z0 = (par_0 + par_1) / SQRT_2
    saida_Z4 = (par_0 - par_1) / SQRT_2
    saida_Z2 = C6 * par_2 + S6 * par_3
    saida_Z6 = -S6 * par_2 + C6 * par_3

    # Estágio 2: Reagrupamento para componentes ímpares
    impar_0, impar_1 = dif_07 + dif_34, dif_16 + dif_25
    impar_2, impar_3 = dif_16 - dif_25, dif_07 - dif_34

    # Estágio 3: Rotações (componentes ímpares)
    saida_Z1 = (C3 * impar_0 + C1 * impar_1 + S1 * impar_2 + S3 * impar_3) / SQRT_2
    saida_Z3 = (S1 * impar_0 - C3 * impar_1 + S3 * impar_2 + C1 * impar_3) / SQRT_2
    saida_Z5 = (C1 * impar_0 - S3 * impar_1 - C3 * impar_2 - S1 * impar_3) / SQRT_2
    saida_Z7 = (-S3 * impar_0 + S1 * impar_1 - C1 * impar_2 + C3 * impar_3) / SQRT_2

    return np.array([saida_Z0, saida_Z1, saida_Z2, saida_Z3, saida_Z4, saida_Z5, saida_Z6, saida_Z7]) / 2.0


def idct_loeffler_1d(coeficientes_dct):
    vetor_Z = np.asarray(coeficientes_dct, dtype=np.float32).flatten() * 2.0 
    Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7 = vetor_Z
    
    # Estágio 1 Inverso: Rotações (componentes pares)
    temp_0, temp_4 = Z0 * SQRT_2, Z4 * SQRT_2
    par_0 = 0.5 * (temp_0 + temp_4)
    par_1 = 0.5 * (temp_0 - temp_4)
    par_2 = C6 * Z2 - S6 * Z6
    par_3 = S6 * Z2 + C6 * Z6

    # Estágio 2 Inverso: Reagrupamento (componentes pares)
    soma_07 = 0.5 * (par_0 + par_3)
    soma_34 = 0.5 * (par_0 - par_3)
    soma_16 = 0.5 * (par_1 + par_2)
    soma_25 = 0.5 * (par_1 - par_2)

    # Estágio 1 Inverso: Rotações (componentes ímpares)
    w1, w3, w5, w7 = Z1, Z3, Z5, Z7
    impar_0 = (C3 * w1 + S1 * w3 + C1 * w5 - S3 * w7) / SQRT_2
    impar_1 = (C1 * w1 - C3 * w3 - S3 * w5 + S1 * w7) / SQRT_2
    impar_2 = (S1 * w1 + S3 * w3 - C3 * w5 - C1 * w7) / SQRT_2
    impar_3 = (S3 * w1 + C1 * w3 - S1 * w5 + C3 * w7) / SQRT_2

    # Estágio 2 Inverso: Reagrupamento (componentes ímpares)
    dif_07 = 0.5 * (impar_0 + impar_3)
    dif_34 = 0.5 * (impar_0 - impar_3)
    dif_16 = 0.5 * (impar_1 + impar_2)
    dif_25 = 0.5 * (impar_1 - impar_2)

    # Estágio 3 Inverso: Borboletas para reconstruir o sinal original
    x0, x7 = 0.5 * (soma_07 + dif_07), 0.5 * (soma_07 - dif_07)
    x1, x6 = 0.5 * (soma_16 + dif_16), 0.5 * (soma_16 - dif_16)
    x2, x5 = 0.5 * (soma_25 + dif_25), 0.5 * (soma_25 - dif_25)
    x3, x4 = 0.5 * (soma_34 + dif_34), 0.5 * (soma_34 - dif_34)
    
    return np.array([x0, x1, x2, x3, x4, x5, x6, x7])


# ----------------- Definição Matricial -----------------
def dct_matrix_1d(vetor_entrada):
    TAMANHO = 8
    coeficientes_dct = np.zeros(TAMANHO, dtype=np.float32) 
    
    for k in range(TAMANHO):
        alpha = 1.0 / SQRT_2 if k == 0 else 1.0

        soma = sum(vetor_entrada[n] * math.cos(math.pi * k * (2 * n + 1) / (2 * TAMANHO)) for n in range(TAMANHO))
        
        coeficientes_dct[k] = alpha * soma
        
    return coeficientes_dct * math.sqrt(2.0 / TAMANHO)


def idct_matrix_1d(coeficientes_dct):
    TAMANHO = 8
    vetor_saida = np.zeros(TAMANHO, dtype=np.float32)
    
    for n in range(TAMANHO):
        soma = 0.0
        for k in range(TAMANHO):
            alpha = 1.0 / SQRT_2 if k == 0 else 1.0
            
            soma += alpha * coeficientes_dct[k] * math.cos(math.pi * k * (2 * n + 1) / (2 * TAMANHO))
            
        vetor_saida[n] = soma
        
    return vetor_saida * math.sqrt(2.0 / TAMANHO)


# ----------------- Funções 2D Aplicáveis a Blocos 8x8 -----------------
def dct_2d(bloco_8x8, funcao_dct_1d):
    dct_nas_linhas = np.array([funcao_dct_1d(linha) for linha in bloco_8x8])
    dct_nas_colunas = np.array([funcao_dct_1d(coluna) for coluna in dct_nas_linhas.T])
    return dct_nas_colunas.T


def idct_2d(bloco_dct_8x8, funcao_idct_1d):
    idct_nas_linhas = np.array([funcao_idct_1d(linha) for linha in bloco_dct_8x8])
    idct_nas_colunas = np.array([funcao_idct_1d(coluna) for coluna in idct_nas_linhas.T])
    return idct_nas_colunas.T