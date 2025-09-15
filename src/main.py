import os
import time
from PIL import Image
import numpy as np

from dct import dct_loeffler_1d, idct_loeffler_1d, dct_matrix_1d, idct_matrix_1d
from pipeline import processar_canal, rgb_para_ycbcr, ycbcr_para_rgb
from plots import (imprimir_e_salvar_resultados, plotar_metricas_vs_fator_k, plotar_barras_comparativas, 
                    plotar_tradeoff_qualidade_vs_tempo, plotar_analise_do_dataset, calcular_metricas_de_qualidade)
from constantes import Q50_LUMA, Q50_CHROMA

# ---------------- CONFIGURAÇÃO GERAL ----------------
METODO_DCT = 'loeffler'  #'loeffler' ou 'matrix'
DIRETORIO_ENTRADA = 'src/imgs'
DIRETORIO_SAIDA_IMAGENS = f'resultados_{METODO_DCT}'
DIRETORIO_SAIDA_PLOTS = f'plots_{METODO_DCT}'
FATORES_DE_COMPRESSAO_K = [2.0, 5.0, 10.0, 15.0]


os.makedirs(DIRETORIO_SAIDA_IMAGENS, exist_ok=True)
os.makedirs(DIRETORIO_SAIDA_PLOTS, exist_ok=True)

# ---------------- SELEÇÃO DO MÉTODO DCT ----------------
if METODO_DCT == 'loeffler':
    funcao_dct_1d, funcao_idct_1d = dct_loeffler_1d, idct_loeffler_1d
else:
    funcao_dct_1d, funcao_idct_1d = dct_matrix_1d, idct_matrix_1d

# ---------------- FUNÇÃO DE PROCESSAMENTO DE IMAGEM ----------------
def processar_imagem(caminho_do_arquivo):
    imagem_original = Image.open(caminho_do_arquivo).convert('RGB')
    matriz_rgb_original = np.array(imagem_original, dtype=np.uint8)
    
    canal_r, canal_g, canal_b = matriz_rgb_original[:,:,0], matriz_rgb_original[:,:,1], matriz_rgb_original[:,:,2]
    
    canal_y, canal_cb, canal_cr = rgb_para_ycbcr(canal_r, canal_g, canal_b)

    lista_de_resultados = []
    tempo_total = 0

    for fator_k in FATORES_DE_COMPRESSAO_K:
        inicio_cronometro = time.perf_counter()
        
        y_reconstruido = processar_canal(canal_y, Q50_LUMA, fator_k, funcao_dct_1d, funcao_idct_1d)
        cb_reconstruido = processar_canal(canal_cb, Q50_CHROMA, fator_k, funcao_dct_1d, funcao_idct_1d)
        cr_reconstruido = processar_canal(canal_cr, Q50_CHROMA, fator_k, funcao_dct_1d, funcao_idct_1d)
        
        tempo_decorrido_ms = (time.perf_counter() - inicio_cronometro) * 1000
        tempo_total += tempo_decorrido_ms

        imagem_reconstruida_rgb = ycbcr_para_rgb(y_reconstruido, cb_reconstruido, cr_reconstruido)
        
        nome_base_arquivo = os.path.basename(caminho_do_arquivo).split('.')[0]
        diretorio_saida_da_imagem = os.path.join(DIRETORIO_SAIDA_IMAGENS, nome_base_arquivo)
        os.makedirs(diretorio_saida_da_imagem, exist_ok=True)
        
        caminho_arquivo_saida = os.path.join(diretorio_saida_da_imagem, f"k={fator_k}.png")
        Image.fromarray(imagem_reconstruida_rgb).save(caminho_arquivo_saida)

        valor_psnr, valor_ssim = calcular_metricas_de_qualidade(matriz_rgb_original, imagem_reconstruida_rgb)
        
        nome_original_arquivo = os.path.basename(caminho_do_arquivo)
        lista_de_resultados.append((fator_k, valor_psnr, valor_ssim, tempo_decorrido_ms, nome_original_arquivo))

    return lista_de_resultados, tempo_total

# ---------------- FUNÇÃO DE PROCESSAMENTO DE DATASET ----------------
def processar_dataset():
    lista_arquivos_imagem = [f for f in os.listdir(DIRETORIO_ENTRADA) if f.lower().endswith(('.png','.jpg','.jpeg', '.bmp'))]
    
    if not lista_arquivos_imagem:
        print(f"Nenhuma imagem encontrada em '{DIRETORIO_ENTRADA}'. Por favor, adicione imagens e tente novamente.")
        return

    resultados_globais_dataset = [] 

    for nome_arquivo in lista_arquivos_imagem:
        caminho_completo = os.path.join(DIRETORIO_ENTRADA, nome_arquivo)
        print(f"\n>>> Processando: {caminho_completo}")
        
        resultados_da_imagem, tempo_total_imagem = processar_imagem(caminho_completo)
        
        resultados_globais_dataset.append(resultados_da_imagem)
        
        imprimir_e_salvar_resultados(
            resultados_da_imagem, 
            tempo_total_imagem, 
            nome_arquivo, 
            output_dir=DIRETORIO_SAIDA_IMAGENS, 
            plot_dir=DIRETORIO_SAIDA_PLOTS
        )
        plotar_metricas_vs_fator_k(resultados_da_imagem, nome_arquivo, DIRETORIO_SAIDA_PLOTS)
        plotar_barras_comparativas(resultados_da_imagem, nome_arquivo, DIRETORIO_SAIDA_PLOTS)
        plotar_tradeoff_qualidade_vs_tempo(resultados_da_imagem, nome_arquivo, DIRETORIO_SAIDA_PLOTS)

    plotar_analise_do_dataset(resultados_globais_dataset, DIRETORIO_SAIDA_PLOTS)

# ---------------- EXECUÇÃO PRINCIPAL DO SCRIPT ----------------
if __name__ == "__main__":
    if not os.path.exists(DIRETORIO_ENTRADA):
        os.makedirs(DIRETORIO_ENTRADA)
    else:
        processar_dataset()