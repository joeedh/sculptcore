import type {MeshLog} from '../data/MeshSource'

export type StepListener = (step: number) => void

export class Timeline {
  private slider: HTMLInputElement
  private label: HTMLElement
  private opLabel: HTMLElement
  private onStep: StepListener
  private log: MeshLog | null = null
  private step = 0

  constructor(onStep: StepListener) {
    this.slider = document.getElementById('step-slider') as HTMLInputElement
    this.label = document.getElementById('step-label')!
    this.opLabel = document.getElementById('op-label')!
    this.onStep = onStep
    this.slider.addEventListener('input', () => {
      this.setStep(parseInt(this.slider.value, 10))
    })
    document.getElementById('step-first')!.addEventListener('click', () => this.setStep(0))
    document.getElementById('step-last')!.addEventListener('click', () => {
      this.setStep(this.maxStep())
    })
    document.getElementById('step-prev')!.addEventListener('click', () => {
      this.setStep(this.step - 1)
    })
    document.getElementById('step-next')!.addEventListener('click', () => {
      this.setStep(this.step + 1)
    })
  }

  private maxStep(): number {
    return this.log ? this.log.steps.length : 0
  }

  setLog(log: MeshLog | null): void {
    this.log = log
    this.step = 0
    this.slider.min = '0'
    this.slider.max = String(this.maxStep())
    this.slider.value = '0'
    this.refreshLabel()
    this.onStep(0)
  }

  private setStep(s: number): void {
    if (s < 0) s = 0
    if (s > this.maxStep()) s = this.maxStep()
    this.step = s
    this.slider.value = String(s)
    this.refreshLabel()
    this.onStep(s)
  }

  private refreshLabel(): void {
    const n = this.maxStep()
    this.label.textContent = `${this.step} / ${n}`
    if (!this.log || this.step === 0) {
      this.opLabel.textContent = this.log ? `[${this.log.tag}] initial` : ''
    } else {
      const st = this.log.steps[this.step - 1]
      const hl = st.highlight ? ` ${st.highlight.kind}=[${st.highlight.ids.join(',')}]` : ''
      this.opLabel.textContent = `[${this.log.tag}] ${st.op}${hl}`
    }
  }
}
