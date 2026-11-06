import { Component } from "@angular/core";
import { AppModule } from "../app.module";

@Component({
  selector: "app-dashboard",
  standalone: true,
  imports: [AppModule],
  templateUrl: "./dashboard.component.html",
  styleUrl: "./dashboard.component.scss",
})
export class DashboardComponent {}
