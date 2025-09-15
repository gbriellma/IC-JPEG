import os
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from skimage.metrics import peak_signal_noise_ratio as calcular_psnr, structural_similarity as calcular_ssim

# ---------------- FUNÇÕES DE MÉTRICAS ----------------
def calcular_metricas_de_qualidade(imagem_original, imagem_reconstruida):
    valor_psnr = calcular_psnr(imagem_original, imagem_reconstruida, data_range=255)
    valor_ssim = calcular_ssim(imagem_original, imagem_reconstruida, data_range=255, channel_axis=-1, win_size=7)
    return valor_psnr, valor_ssim

# ---------------- FUNÇÕES DE SAÍDA (OUTPUT) ----------------
def imprimir_e_salvar_resultados(lista_de_resultados, tempo_total, nome_da_imagem, output_dir=None, plot_dir=None):
    linhas_de_saida = []
    linhas_de_saida.append(f"--- RESULTADOS FINAIS: {nome_da_imagem} ---")
    linhas_de_saida.append("┌─────────┬───────────┬─────────┬──────────────────────────────┐")
    linhas_de_saida.append("│ Fator k │ PSNR (dB) │  SSIM   │ Tempo (ms | s | min)        │")
    linhas_de_saida.append("├─────────┼───────────┼─────────┼──────────────────────────────┤")
    for resultado_tupla in lista_de_resultados:
        fator_k, valor_psnr, valor_ssim, tempo, _ = resultado_tupla
        tempo_s = tempo / 1000
        tempo_min = tempo_s / 60
        linhas_de_saida.append(f"│ {fator_k:>6.1f}  │  {valor_psnr:7.2f}  │ {valor_ssim:.4f}  │ {tempo:>8.2f} | {tempo_s:>6.2f} | {tempo_min:>6.2f} │")
    linhas_de_saida.append("└─────────┴───────────┴─────────┴──────────────────────────────┘")
    tempo_total_s = tempo_total / 1000
    tempo_total_min = tempo_total_s / 60
    linhas_de_saida.append(f"\nTempo total de processamento: {tempo_total:.2f} ms | {tempo_total_s:.2f} s | {tempo_total_min:.2f} min")

    string_de_saida_completa = "\n".join(linhas_de_saida)

    print("\n" + string_de_saida_completa)
    
    if plot_dir:
        caminho_subdiretorio_plot = os.path.join(plot_dir, nome_da_imagem.split('.')[0])
        os.makedirs(caminho_subdiretorio_plot, exist_ok=True)
        
        caminho_arquivo_txt = os.path.join(caminho_subdiretorio_plot, f"resultados_{nome_da_imagem.split('.')[0]}.txt")
        
        with open(caminho_arquivo_txt, 'w', encoding='utf-8') as arquivo:
            arquivo.write(string_de_saida_completa)
        print(f"Resultados também salvos em: '{caminho_arquivo_txt}'")


# ---------------- FUNÇÕES DE PLOTAGEM ----------------
def plotar_metricas_vs_fator_k(lista_de_resultados, nome_da_imagem, diretorio_saida='plots'):
    fatores_k = [res[0] for res in lista_de_resultados]
    valores_psnr = [res[1] for res in lista_de_resultados]
    valores_ssim = [res[2] for res in lista_de_resultados]
    
    caminho_subdiretorio_plot = os.path.join(diretorio_saida, nome_da_imagem.split('.')[0])
    os.makedirs(caminho_subdiretorio_plot, exist_ok=True)

    figura, eixo_psnr = plt.subplots(figsize=(8,5))
    
    cor_psnr = 'tab:blue'
    eixo_psnr.set_xlabel('Fator k')
    eixo_psnr.set_ylabel('PSNR (dB)', color=cor_psnr)
    eixo_psnr.plot(fatores_k, valores_psnr, marker='o', color=cor_psnr, label='PSNR')
    eixo_psnr.tick_params(axis='y', labelcolor=cor_psnr)
    eixo_psnr.grid(True, linestyle='--', alpha=0.5)

    eixo_ssim = eixo_psnr.twinx()
    cor_ssim = 'tab:green'
    eixo_ssim.set_ylabel('SSIM', color=cor_ssim)
    eixo_ssim.plot(fatores_k, valores_ssim, marker='s', color=cor_ssim, label='SSIM')
    eixo_ssim.tick_params(axis='y', labelcolor=cor_ssim)

    plt.title(f"Desempenho da Compressão: {nome_da_imagem}")
    figura.tight_layout()
    plt.savefig(os.path.join(caminho_subdiretorio_plot, f'{nome_da_imagem}_metricas_linha.png'))
    plt.close()

def plotar_barras_comparativas(lista_de_resultados, nome_da_imagem, diretorio_saida='plots'):
    fatores_k = [str(res[0]) for res in lista_de_resultados]
    valores_psnr = [res[1] for res in lista_de_resultados]
    valores_ssim = [res[2] for res in lista_de_resultados]

    caminho_subdiretorio_plot = os.path.join(diretorio_saida, nome_da_imagem.split('.')[0])
    os.makedirs(caminho_subdiretorio_plot, exist_ok=True)

    plt.figure(figsize=(7, 5))
    plt.bar(fatores_k, valores_psnr, color='skyblue')
    plt.xlabel('Fator k')
    plt.ylabel('PSNR (dB)')
    plt.title(f'PSNR por Fator k: {nome_da_imagem}')
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    plt.savefig(os.path.join(caminho_subdiretorio_plot, f'{nome_da_imagem}_psnr_barras.png'))
    plt.close()

    plt.figure(figsize=(7, 5))
    plt.bar(fatores_k, valores_ssim, color='lightgreen')
    plt.xlabel('Fator k')
    plt.ylabel('SSIM')
    plt.ylim(bottom=min(valores_ssim) * 0.95, top=1.0)
    plt.title(f'SSIM por Fator k: {nome_da_imagem}')
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    plt.savefig(os.path.join(caminho_subdiretorio_plot, f'{nome_da_imagem}_ssim_barras.png'))
    plt.close()

def plotar_tradeoff_qualidade_vs_tempo(lista_de_resultados, nome_da_imagem, diretorio_saida='plots'):
    fatores_k = [res[0] for res in lista_de_resultados]
    valores_psnr = [res[1] for res in lista_de_resultados]
    tempos_ms = [res[3] for res in lista_de_resultados]
    
    caminho_subdiretorio_plot = os.path.join(diretorio_saida, nome_da_imagem.split('.')[0])
    os.makedirs(caminho_subdiretorio_plot, exist_ok=True)

    plt.figure(figsize=(8, 6))
    plt.scatter(tempos_ms, valores_psnr, c=fatores_k, cmap='viridis', s=100, edgecolors='k', alpha=0.75)
    plt.colorbar(label='Fator k')

    for indice, k in enumerate(fatores_k):
        plt.annotate(f'k={k}', (tempos_ms[indice], valores_psnr[indice]), textcoords="offset points", xytext=(0,10), ha='center')

    plt.xlabel('Tempo de Processamento (ms)')
    plt.ylabel('PSNR (dB)')
    plt.title(f'Trade-off Qualidade vs. Tempo: {nome_da_imagem}')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(caminho_subdiretorio_plot, f'{nome_da_imagem}_tradeoff.png'))
    plt.close()

def plotar_analise_do_dataset(resultados_globais, diretorio_saida='plots'):
    if not resultados_globais:
        print("Nenhum resultado global para plotar a análise do dataset.")
        return

    lista_plana_resultados = [item for sublista in resultados_globais for item in sublista]
    dataframe_resultados = pd.DataFrame(lista_plana_resultados, columns=['k', 'PSNR', 'SSIM', 'Tempo', 'Imagem'])

    caminho_diretorio_analise = os.path.join(diretorio_saida, '_analise_dataset')
    os.makedirs(caminho_diretorio_analise, exist_ok=True)
    
    fatores_k_unicos = sorted(dataframe_resultados['k'].unique())

    for nome_metrica in ['PSNR', 'SSIM', 'Tempo']:
        plt.figure(figsize=(8, 6))
        
        dados_para_plotagem = [dataframe_resultados[dataframe_resultados['k'] == k][nome_metrica].values for k in fatores_k_unicos]

        plt.boxplot(dados_para_plotagem, labels=[str(k) for k in fatores_k_unicos])
        
        plt.title(f'Distribuição de {nome_metrica} por Fator k (Dataset Completo)')
        plt.xlabel('Fator k')
        unidade = ' (dB)' if nome_metrica == 'PSNR' else ' (ms)' if nome_metrica == 'Tempo' else ''
        plt.ylabel(f'{nome_metrica}{unidade}')
        plt.grid(axis='y', linestyle='--', alpha=0.7)
        plt.savefig(os.path.join(caminho_diretorio_analise, f'dataset_{nome_metrica}_boxplot.png'))
        plt.close()
    
    print(f"\nAnálise do dataset concluída. plots salvos em '{caminho_diretorio_analise}/'")