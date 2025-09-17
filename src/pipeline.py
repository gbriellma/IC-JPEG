import numpy as np
from dct import dct_2d, idct_2d

TIPO_CONSTANTES = np.float64


def dividir_em_blocos(canal_imagem):
    altura_original, largura_original = canal_imagem.shape
    
    preenchimento_altura = (8 - altura_original % 8) % 8
    preenchimento_largura = (8 - largura_original % 8) % 8
    
    canal_preenchido = np.pad(canal_imagem, ((0, preenchimento_altura), (0, preenchimento_largura)), mode='edge')
    
    altura_preenchida, largura_preenchida = canal_preenchido.shape
    
    blocos = canal_preenchido.reshape(altura_preenchida // 8, 8, largura_preenchida // 8, 8).swapaxes(1, 2).reshape(-1, 8, 8)
    
    return blocos, (altura_preenchida, largura_preenchida), (altura_original, largura_original)

def juntar_blocos(lista_de_blocos, dimensao_preenchida, dimensao_original):
    altura_preenchida, largura_preenchida = dimensao_preenchida
    altura_original, largura_original = dimensao_original
    
    canal_completo = lista_de_blocos.reshape(altura_preenchida // 8, largura_preenchida // 8, 8, 8).swapaxes(1, 2).reshape(altura_preenchida, largura_preenchida)

    return canal_completo[:altura_original, :largura_original]

def processar_canal(canal_imagem, tabela_quantizacao, fator_k, funcao_dct_1d, funcao_idct_1d):
    canal_deslocado = canal_imagem.astype(TIPO_CONSTANTES) - 128.0
    
    blocos, dimensao_preenchida, dimensao_original = dividir_em_blocos(canal_deslocado)

    tabela_quantizacao_escalonada = tabela_quantizacao * (fator_k)
    
    blocos_processados = []

    for bloco in blocos:
        bloco_dct = dct_2d(bloco, funcao_dct_1d)
        bloco_quantizado = np.round(bloco_dct / tabela_quantizacao_escalonada)
        bloco_dequantizado = bloco_quantizado * tabela_quantizacao_escalonada
        bloco_idct = idct_2d(bloco_dequantizado, funcao_idct_1d)
        blocos_processados.append(bloco_idct)
    
    canal_reconstruido = juntar_blocos(np.array(blocos_processados), dimensao_preenchida, dimensao_original) + 128.0
    
    return canal_reconstruido


# --- Funções de Conversão de Espaço de Cores ---
def rgb_para_ycbcr(canal_r, canal_g, canal_b):
    canal_y = 0.299*canal_r + 0.587*canal_g + 0.114*canal_b
    canal_cb = 128.0 + (-0.168736*canal_r - 0.331264*canal_g + 0.5*canal_b)
    canal_cr = 128.0 + (0.5*canal_r - 0.418688*canal_g - 0.081312*canal_b)
    return canal_y, canal_cb, canal_cr

def ycbcr_para_rgb(canal_y, canal_cb, canal_cr):
    canal_r = canal_y + 1.402*(canal_cr - 128.0)
    canal_g = canal_y - 0.344136*(canal_cb - 128.0) - 0.714136*(canal_cr - 128.0)
    canal_b = canal_y + 1.772*(canal_cb - 128.0)

    matriz_rgb = np.stack([canal_r, canal_g, canal_b], axis=-1)
    
    return np.clip(matriz_rgb, 0, 255).astype(TIPO_CONSTANTES)